#include "appproxy.h"

#include <algorithm>
#include <cstdlib>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QSysInfo>

#include "constants.h"
#include "networkproxy.h"
#include "networkrequest.h"

AppProxy::AppProxy(QObject* parent)
  : QObject(parent),
    v2ray(V2RayCore::getInstance()),
    configurator(Configurator::getInstance()) {
  // Setup Worker
  worker->moveToThread(&workerThread);
  connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);

  // Setup Worker -> getServerLatency
  connect(this, &AppProxy::getServerLatencyStarted, worker,
          &AppProxyWorker::getServerLatency);
  connect(worker, &AppProxyWorker::serverLatencyReady, this,
          &AppProxy::returnServerLatency);

  // Setup Worker -> getGfwList
  connect(this, &AppProxy::getGfwListStarted, worker,
          &AppProxyWorker::getGfwList);
  connect(worker, &AppProxyWorker::gfwListReady, this,
          &AppProxy::returnGfwList);

  // Setup Worker -> getNetworkStatus
  connect(this, &AppProxy::getNetworkStatusStarted, worker,
          &AppProxyWorker::getUrlAccessibility);
  connect(worker, &AppProxyWorker::urlAccessibilityReady, this,
          &AppProxy::returnNetworkAccessiblity);

  workerThread.start();
}

AppProxy::~AppProxy() {
  workerThread.quit();
  workerThread.wait();
}

void AppProxy::getAppVersion() {
  QString appVersion = QString("v%1.%2.%3")
                         .arg(QString::number(APP_VERSION_MAJOR),
                              QString::number(APP_VERSION_MINOR),
                              QString::number(APP_VERSION_PATCH));
  emit appVersionReady(appVersion);
}

void AppProxy::getV2RayCoreVersion() {
  QJsonObject appConfig    = configurator.getAppConfig();
  QString v2RayCoreVersion = appConfig["v2rayCoreVersion"].toString();
  emit v2RayCoreVersionReady(v2RayCoreVersion);
}

void AppProxy::getOperatingSystem() {
  QString operatingSystem = QSysInfo::prettyProductName();
  emit operatingSystemReady(operatingSystem);
}

void AppProxy::getV2RayCoreStatus() {
  bool isInstalled = v2ray.isInstalled();
  bool isRunning   = v2ray.isRunning();
  QString v2rayStatus =
    isInstalled ? (isRunning ? "Running" : "Stopped") : "Not Installed";
  emit v2RayCoreStatusReady(v2rayStatus);
}

void AppProxy::setV2RayCoreRunning(bool expectedRunning) {
  bool isSuccessful = false;
  if (expectedRunning) {
    isSuccessful = v2ray.start();
    qInfo() << QString("Start V2Ray Core ... %1")
                 .arg(isSuccessful ? "success" : "failed");
    emit v2RayRunningStatusChanging(isSuccessful);
  } else {
    isSuccessful = v2ray.stop();
    qInfo() << QString("Stop V2Ray Core ... %1")
                 .arg(isSuccessful ? "success" : "failed");
    emit v2RayRunningStatusChanging(isSuccessful);
  }
}

void AppProxy::getNetworkStatus() {
  qRegisterMetaType<QMap<QString, bool>>("QMap");
  qRegisterMetaType<QNetworkProxy>("QNetworkProxy");
  emit getNetworkStatusStarted({{"google.com", true}, {"baidu.com", false}},
                               getQProxy());
}

QNetworkProxy AppProxy::getQProxy() {
  QJsonObject appConfig = configurator.getAppConfig();
  QNetworkProxy::ProxyType proxyType =
    appConfig["serverProtocol"].toString() == "SOCKS"
      ? QNetworkProxy::Socks5Proxy
      : QNetworkProxy::HttpProxy;
  int serverPort = appConfig["serverPort"].toInt();
  QNetworkProxy proxy;
  proxy.setType(proxyType);
  proxy.setHostName("127.0.0.1");
  proxy.setPort(serverPort);
  return proxy;
}

void AppProxy::returnNetworkAccessiblity(QMap<QString, bool> accessible) {
  bool isGoogleAccessible =
         accessible.contains("google.com") ? accessible["google.com"] : false,
       isBaiduAccessible =
         accessible.contains("baidu.com") ? accessible["baidu.com"] : false;

  emit networkStatusReady(
    QJsonDocument(QJsonObject{
                    {"isGoogleAccessible", isGoogleAccessible},
                    {"isBaiduAccessible", isBaiduAccessible},
                  })
      .toJson());
}

void AppProxy::getAppConfig() {
  QJsonObject appConfig = configurator.getAppConfig();
  emit appConfigReady(QJsonDocument(appConfig).toJson());
}

void AppProxy::saveAppConfig(QString configString) {
  QJsonDocument configDoc = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject appConfig   = configDoc.object();
  // TODO: Check app config before saving
  // Save app config
  configurator.setAppConfig(appConfig);
  emit appConfigChanged();
  qInfo() << "Application config updated. Restarting V2Ray ...";
  // Restart V2Ray Core
  v2ray.restart();
}

void AppProxy::getLogs() {
  QFile appLogFile(Configurator::getAppLogFilePath());
  QFile v2RayLogFile(Configurator::getV2RayLogFilePath());
  QStringList logs;
  // Read the app and V2Ray logs
  if (appLogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QList<QByteArray> _logList = appLogFile.readAll().split('\n');
    int cnt                    = 0;
    for (auto itr = _logList.end() - 1;
         itr >= _logList.begin() && cnt <= MAX_N_LOGS; --itr, ++cnt) {
      logs.append(*itr);
    }
  }
  if (v2RayLogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QList<QByteArray> _logList = v2RayLogFile.readAll().split('\n');
    int cnt                    = 0;
    for (auto itr = _logList.end() - 1;
         itr >= _logList.begin() && cnt <= MAX_N_LOGS; --itr, ++cnt) {
      logs.append(*itr);
    }
  }
  // Sort logs by timestamp
  logs.sort();
  std::reverse(logs.begin(), logs.end());

  QString _logs = logs.join('\n');
  emit logsReady(_logs);
}

void AppProxy::clearLogs() {
  QFile appLogFile(Configurator::getAppLogFilePath());
  QFile v2RayLogFile(Configurator::getV2RayLogFilePath());
  if (appLogFile.exists()) {
    appLogFile.resize(0);
  }
  if (v2RayLogFile.exists()) {
    v2RayLogFile.resize(0);
  }
}

void AppProxy::getProxySettings() {
  bool isV2RayRunning     = v2ray.isRunning();
  bool isPacServerRunning = pacServer.isRunning();

  QString proxyMode = NetworkProxyHelper::getSystemProxy().toString();
  QStringList connectedServers = configurator.getConnectedServerNames();
  emit proxySettingsReady(
    QJsonDocument(
      QJsonObject{{"isV2RayRunning", isV2RayRunning},
                  {"isPacServerRunning", isPacServerRunning},
                  {"proxyMode", proxyMode},
                  {"connectedServers", connectedServers.join(", ")}})
      .toJson());
}

void AppProxy::setSystemProxyMode(QString proxyMode) {
  QJsonObject appConfig = configurator.getAppConfig();
  // Automatically set system proxy according to app config
  if (!proxyMode.size()) {
    proxyMode = appConfig["proxyMode"].toString();
  }

  // Set system proxy
  NetworkProxy proxy;
  proxy.host = "127.0.0.1";
  NetworkProxyHelper::resetSystemProxy();
  if (pacServer.isRunning()) {
    pacServer.stop();
  }
  if (proxyMode == "global") {
    QString protocol = appConfig["serverProtocol"].toString();
    proxy.port       = appConfig["serverPort"].toInt();
    proxy.type       = protocol == "SOCKS" ? NetworkProxyType::SOCKS_PROXY
                                     : NetworkProxyType::HTTP_PROXY;
  } else if (proxyMode == "pac") {
    proxy.port = appConfig["pacPort"].toInt();
    proxy.type = NetworkProxyType::PAC_PROXY;
    proxy.url  = QString("http://%1:%2/proxy.pac")
                  .arg(proxy.host, QString::number(proxy.port));
    // Restart PAC Server
    QString pacServerHost = appConfig["serverIp"].toString();
    pacServer.start(pacServerHost, proxy.port);
  }
  NetworkProxyHelper::setSystemProxy(proxy);
  emit proxyModeChanged(proxyMode);

  // Update app config
  configurator.setAppConfig({{"proxyMode", proxyMode}});
}

void AppProxy::updateGfwList(QString gfwListUrl) {
  emit getGfwListStarted(gfwListUrl);
}

void AppProxy::returnGfwList(QByteArray gfwList) {
  if (gfwList.size()) {
    QFile gfwListFile(Configurator::getGfwListFilePath());
    gfwListFile.open(QFile::WriteOnly);
    gfwListFile.write(gfwList);
    gfwListFile.flush();
    // Update app config
    QString updatedTime = QDateTime::currentDateTime().toString();
    configurator.setAppConfig(QJsonObject{
      {"gfwListLastUpdated", QDateTime::currentDateTime().toString()}});
    qInfo() << "GFW List updated successfully.";
    emit gfwListUpdated(updatedTime);
  } else {
    emit gfwListUpdated("Failed to update GFW List.");
  }
}

void AppProxy::getServers() {
  QJsonArray servers               = configurator.getServers();
  QStringList connectedServerNames = configurator.getConnectedServerNames();

  for (auto itr = servers.begin(); itr != servers.end(); ++itr) {
    QJsonObject server = (*itr).toObject();
    QString serverName =
      server.contains("serverName") ? server["serverName"].toString() : "";
    server["connected"] = connectedServerNames.contains(serverName);
    if (serverLatency.contains(serverName)) {
      server["latency"] = serverLatency[serverName].toInt();
    }
    *itr = server;
  }
  emit serversReady(QJsonDocument(servers).toJson());
}

void AppProxy::getServer(QString serverName, bool forDuplicate) {
  QJsonObject server = configurator.getServer(serverName);
  if (forDuplicate) {
    server.remove("serverName");
  }
  emit serverDInfoReady(QJsonDocument(server).toJson());
}

void AppProxy::getServerLatency(QString serverName) {
  QJsonObject _serverLatency;
  QJsonArray servers;
  if (serverName.size()) {
    servers.append(configurator.getServer(serverName));
  } else {
    servers = configurator.getServers();
  }
  qRegisterMetaType<QJsonArray>("QJsonArray");
  emit getServerLatencyStarted(servers);
}

void AppProxy::returnServerLatency(QMap<QString, QVariant> latency) {
  // Ref:
  // https://stackoverflow.com/questions/8517853/iterating-over-a-qmap-with-for
  // Note: Convert to StdMap for better performance
  for (auto l : latency.toStdMap()) {
    serverLatency[l.first] = l.second.toInt();
  }
  emit serverLatencyReady(QJsonDocument::fromVariant(latency).toJson());
}

void AppProxy::setServerConnection(QString serverName, bool connected) {
  configurator.setServerConnection(serverName, connected);
  v2ray.restart();
  qInfo() << (connected ? "Connected to " : "Disconnected from ") << serverName;
  emit serversChanged();
}

void AppProxy::addV2RayServer(QString configString) {
  QJsonDocument configDoc  = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject serverConfig = configDoc.object();
  // TODO: Check server config before saving
  // Save server config
  configurator.addServer(getPrettyV2RayConfig(serverConfig));
  emit serversChanged();
  qInfo() << QString("Add new V2Ray server [Name=%1, Addr=%2].")
               .arg(serverConfig["serverName"].toString(),
                    serverConfig["serverAddr"].toString());
}

QJsonObject AppProxy::getPrettyV2RayConfig(const QJsonObject& serverConfig) {
  QJsonObject v2RayConfig{
    {"autoConnect", serverConfig["autoConnect"].toBool()},
    {"serverName", serverConfig["serverName"].toString()},
    {"protocol", "vmess"},
    {"settings",
     QJsonObject{
       {"vnext", QJsonArray{QJsonObject{
                   {"address", serverConfig["serverAddr"].toString()},
                   {"port", serverConfig["serverPort"].toInt()},
                   {"users", QJsonArray{QJsonObject{
                               {"id", serverConfig["id"].toString()},
                               {"alterId", serverConfig["alterId"].toInt()},
                               {"level", serverConfig["level"].toInt()},
                               {"security",
                                serverConfig["security"].toString().toLower()},
                             }}}}}}}},
    {"tag", "proxy-vmess"}};

  QJsonObject streamSettings = getV2RayStreamSettingsConfig(serverConfig);
  v2RayConfig.insert("streamSettings", streamSettings);
  return v2RayConfig;
}

QJsonObject AppProxy::getV2RayStreamSettingsConfig(
  const QJsonObject& serverConfig) {
  QString network = serverConfig["network"].toString();
  QJsonObject streamSettings{
    {"network", serverConfig["network"]},
    {"security", serverConfig["networkSecurity"].toString().toLower()},
    {"tlsSettings",
     QJsonObject{{"allowInsecure", serverConfig["allowInsecure"].toBool()}}},
  };

  if (network == "tcp") {
    QString tcpHeaderType = serverConfig["tcpHeaderType"].toString().toLower();
    QJsonObject tcpSettings{{"type", tcpHeaderType}};
    if (tcpHeaderType == "http") {
      tcpSettings.insert(
        "request",
        QJsonObject{
          {"version", "1.1"},
          {"method", "GET"},
          {"path", QJsonArray{"/"}},
          {"headers",
           QJsonObject{
             {"host",
              QJsonArray{"www.baidu.com", "www.bing.com", "www.163.com",
                         "www.netease.com", "www.qq.com", "www.tencent.com",
                         "www.taobao.com", "www.tmall.com",
                         "www.alibaba-inc.com", "www.aliyun.com",
                         "www.sensetime.com", "www.megvii.com"}},
             {"User-Agent", getRandomUserAgents(24)},
             {"Accept-Encoding", QJsonArray{"gzip, deflate"}},
             {"Connection", QJsonArray{"keep-alive"}},
             {"Pragma", "no-cache"},
           }},
        });
      tcpSettings.insert(
        "response",
        QJsonObject{
          {"version", "1.1"},
          {"status", "200"},
          {"reason", "OK"},
          {"headers",
           QJsonObject{{"Content-Type", QJsonArray{"text/html;charset=utf-8"}},
                       {"Transfer-Encoding", QJsonArray{"chunked"}},
                       {"Connection", QJsonArray{"keep-alive"}},
                       {"Pragma", "no-cache"}}}});
    }
    streamSettings.insert("tcpSettings", tcpSettings);
  } else if (network == "kcp") {
    streamSettings.insert(
      "kcpSettings",
      QJsonObject{
        {"mtu", serverConfig["kcpMtu"].toInt()},
        {"tti", serverConfig["kcpTti"].toInt()},
        {"uplinkCapacity", serverConfig["kcpUpLink"].toInt()},
        {"downlinkCapacity", serverConfig["kcpDownLink"].toInt()},
        {"congestion", serverConfig["kcpCongestion"].toBool()},
        {"readBufferSize", serverConfig["kcpReadBuffer"].toInt()},
        {"writeBufferSize", serverConfig["kcpWriteBuffer"].toInt()},
        {"header",
         QJsonObject{
           {"type", serverConfig["packetHeader"].toString().toLower()}}}});
  } else if (network == "ws") {
    streamSettings.insert(
      "wsSettings",
      QJsonObject{
        {"path", serverConfig["networkPath"].toString()},
        {"headers", QJsonObject{{"host", serverConfig["serverAddr"]}}}});
  } else if (network == "http") {
    streamSettings.insert(
      "httpSettings",
      QJsonObject{
        {"host", QJsonArray{serverConfig["serverAddr"].toString()}},
        {"path", QJsonArray{serverConfig["networkPath"].toString()}}});
  } else if (network == "domainsocket") {
    streamSettings.insert(
      "dsSettings",
      QJsonObject{{"path", serverConfig["domainSocketFilePath"].toString()}});
  } else if (network == "quic") {
    streamSettings.insert(
      "quicSettings",
      QJsonObject{
        {"security", serverConfig["quicSecurity"].toString().toLower()},
        {"key", serverConfig["quicKey"].toString()},
        {"header",
         QJsonObject{
           {"type", serverConfig["packetHeader"].toString().toLower()}}}});
  }
  return streamSettings;
}

QJsonArray AppProxy::getRandomUserAgents(int n) {
  QStringList OPERATING_SYSTEMS{"Macintosh; Intel Mac OS X 10_15",
                                "X11; Linux x86_64",
                                "Windows NT 10.0; Win64; x64"};
  QJsonArray userAgents;
  for (int i = 0; i < n; ++i) {
    int osIndex            = std::rand() % 3;
    int chromeMajorVersion = std::rand() % 30 + 50;
    int chromeBuildVersion = std::rand() % 4000 + 1000;
    int chromePatchVersion = std::rand() % 100;
    userAgents.append(QString("Mozilla/5.0 (%1) AppleWebKit/537.36 (KHTML, "
                              "like Gecko) Chrome/%2.0.%3.%4 Safari/537.36")
                        .arg(OPERATING_SYSTEMS[osIndex],
                             QString::number(chromeMajorVersion),
                             QString::number(chromeBuildVersion),
                             QString::number(chromePatchVersion)));
  }
  return userAgents;
}

void AppProxy::addShadowsocksServer(QString configString) {
  QJsonDocument configDoc  = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject serverConfig = configDoc.object();
  // TODO: Check server config before saving
  // Save server config
  configurator.addServer(getPrettyShadowsocksConfig(serverConfig));
  emit serversChanged();
  qInfo() << QString("Add new Shadowsocks server [Name=%1, Addr=%2].")
               .arg(serverConfig["serverName"].toString(),
                    serverConfig["serverAddr"].toString());
}

QJsonObject AppProxy::getPrettyShadowsocksConfig(
  const QJsonObject& serverConfig) {
  return QJsonObject{
    {"autoConnect", serverConfig["autoConnect"].toBool()},
    {"serverName", serverConfig["serverName"].toString()},
    {"protocol", "shadowsocks"},
    {"settings",
     QJsonObject{{"servers",
                  QJsonArray{QJsonObject{
                    {"address", serverConfig["serverAddr"].toString()},
                    {"port", serverConfig["serverPort"].toInt()},
                    {"method", serverConfig["encryption"].toString().toLower()},
                    {"password", serverConfig["password"].toString()}}}}}},
    {"streamSettings", QJsonObject{{"network", "tcp"}}},
    {"tag", "proxy-shadowsocks"}};
}

void AppProxy::addSubscriptionUrl(QString subsriptionUrl) {}

void AppProxy::addServerConfigFile(QString configFilePath) {}

void AppProxy::editServer(QString serverName,
                          QString protocol,
                          QString configString) {
  QJsonDocument configDoc  = QJsonDocument::fromJson(configString.toUtf8());
  QJsonObject serverConfig = configDoc.object();
  if (protocol == "vmess") {
    serverConfig = getPrettyV2RayConfig(serverConfig);
  } else if (protocol == "shadowsocks") {
    serverConfig = getPrettyShadowsocksConfig(serverConfig);
  }
  bool isEdited = configurator.editServer(serverName, serverConfig);
  if (isEdited > 0) {
    emit serversChanged();
  }
  // Restart V2Ray Core
  v2ray.restart();
}

void AppProxy::removeServer(QString serverName) {
  configurator.removeServer(serverName);
  qInfo() << QString("Server [Name=%1] have been removed.").arg(serverName);
  emit serversChanged();
  // Restart V2Ray Core
  v2ray.restart();
}
