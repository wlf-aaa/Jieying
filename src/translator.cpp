#include "translator.h"

#include "config.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimeZone>
#include <QTimer>

namespace {

constexpr int kChunkChars = 1800;     // well under both providers' per-request limits
constexpr int kBaiduIntervalMs = 1100; // the free tier allows one request per second
const QUrl kBaiduUrl(QStringLiteral("https://fanyi-api.baidu.com/api/trans/vip/translate"));
const QUrl kTencentUrl(QStringLiteral("https://tmt.tencentcloudapi.com/"));
const QByteArray kTencentHost = "tmt.tencentcloudapi.com";
const QByteArray kTencentContentType = "application/json; charset=utf-8";

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

} // namespace

namespace Translate {

QString targetFor(const QString &text)
{
    int cjk = 0, latin = 0;
    for (const QChar c : text) {
        if (c.unicode() >= 0x4E00 && c.unicode() <= 0x9FFF)
            ++cjk;
        else if (c.isLetter() && c.unicode() < 0x250)
            ++latin;
    }
    // A CJK character carries about as much as a short Latin word.
    return cjk * 3 >= latin ? QStringLiteral("en") : QStringLiteral("zh");
}

QStringList chunks(const QString &text, int maxChars)
{
    QStringList result;
    QString current;
    for (QString line : text.split(QLatin1Char('\n'))) {
        while (line.size() > maxChars) { // one huge line: cut it
            if (!current.isEmpty()) {
                result << current;
                current.clear();
            }
            result << line.left(maxChars);
            line = line.mid(maxChars);
        }
        if (!current.isEmpty() && current.size() + 1 + line.size() > maxChars) {
            result << current;
            current.clear();
        }
        current += current.isEmpty() ? line : QLatin1Char('\n') + line;
    }
    if (!current.trimmed().isEmpty())
        result << current;
    return result;
}

QUrlQuery baiduForm(const QString &appId, const QString &key, const QString &text, const QString &to,
                    const QString &salt)
{
    const QByteArray sign =
        QCryptographicHash::hash((appId + text + salt + key).toUtf8(), QCryptographicHash::Md5).toHex();
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("q"), text);
    form.addQueryItem(QStringLiteral("from"), QStringLiteral("auto"));
    form.addQueryItem(QStringLiteral("to"), to);
    form.addQueryItem(QStringLiteral("appid"), appId);
    form.addQueryItem(QStringLiteral("salt"), salt);
    form.addQueryItem(QStringLiteral("sign"), QString::fromLatin1(sign));
    return form;
}

QString parseBaidu(const QByteArray &json, QString *error)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.contains(QStringLiteral("error_code"))) {
        const QString code = root.value(QStringLiteral("error_code")).toVariant().toString();
        QString hint;
        if (code == QLatin1String("52003") || code == QLatin1String("54001"))
            hint = QStringLiteral("（AppID 或密钥不对）");
        else if (code == QLatin1String("54003"))
            hint = QStringLiteral("（请求太频繁）");
        else if (code == QLatin1String("54004"))
            hint = QStringLiteral("（账户余额不足 / 免费额度用完）");
        *error = QStringLiteral("百度翻译错误 %1：%2%3").arg(code, root.value(QStringLiteral("error_msg")).toString(), hint);
        return {};
    }
    const QJsonArray results = root.value(QStringLiteral("trans_result")).toArray();
    if (results.isEmpty()) {
        *error = QStringLiteral("百度翻译返回了无法识别的内容");
        return {};
    }
    QStringList lines;
    for (const QJsonValue &item : results)
        lines << item.toObject().value(QStringLiteral("dst")).toString();
    return lines.join(QLatin1Char('\n'));
}

QByteArray tencentBody(const QString &text, const QString &to)
{
    QJsonObject body;
    body.insert(QStringLiteral("SourceText"), text);
    body.insert(QStringLiteral("Source"), QStringLiteral("auto"));
    body.insert(QStringLiteral("Target"), to);
    body.insert(QStringLiteral("ProjectId"), 0);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString tencentAuthorization(const QString &secretId, const QString &secretKey, const QByteArray &body,
                             qint64 timestamp)
{
    const QByteArray signedHeaders = "content-type;host;x-tc-action";
    const QByteArray canonicalRequest = "POST\n/\n\n"
                                        "content-type:" + kTencentContentType + "\n"
                                        "host:" + kTencentHost + "\n"
                                        "x-tc-action:texttranslate\n\n"
                                        + signedHeaders + "\n" + sha256Hex(body);
    const QByteArray date = QDateTime::fromSecsSinceEpoch(timestamp, QTimeZone::UTC).toString("yyyy-MM-dd").toLatin1();
    const QByteArray scope = date + "/tmt/tc3_request";
    const QByteArray stringToSign =
        "TC3-HMAC-SHA256\n" + QByteArray::number(timestamp) + "\n" + scope + "\n" + sha256Hex(canonicalRequest);
    const QByteArray secretDate = hmac("TC3" + secretKey.toUtf8(), date);
    const QByteArray secretService = hmac(secretDate, "tmt");
    const QByteArray secretSigning = hmac(secretService, "tc3_request");
    const QByteArray signature = hmac(secretSigning, stringToSign).toHex();
    return QStringLiteral("TC3-HMAC-SHA256 Credential=%1/%2, SignedHeaders=%3, Signature=%4")
        .arg(secretId, QString::fromLatin1(scope), QString::fromLatin1(signedHeaders), QString::fromLatin1(signature));
}

QString parseTencent(const QByteArray &json, QString *error)
{
    const QJsonObject response = QJsonDocument::fromJson(json).object().value(QStringLiteral("Response")).toObject();
    const QJsonObject failure = response.value(QStringLiteral("Error")).toObject();
    if (!failure.isEmpty()) {
        const QString code = failure.value(QStringLiteral("Code")).toString();
        QString hint;
        if (code.startsWith(QLatin1String("AuthFailure")))
            hint = QStringLiteral("（SecretId 或 SecretKey 不对）");
        else if (code.contains(QLatin1String("LimitExceeded")) || code.contains(QLatin1String("NoFreeAmount")))
            hint = QStringLiteral("（额度用完或请求太频繁）");
        else if (code.contains(QLatin1String("ServiceNotOpen")) || code.contains(QLatin1String("UnOpened")))
            hint = QStringLiteral("（需要先在腾讯云控制台开通机器翻译）");
        *error = QStringLiteral("腾讯云翻译错误 %1：%2%3").arg(code, failure.value(QStringLiteral("Message")).toString(), hint);
        return {};
    }
    if (!response.contains(QStringLiteral("TargetText"))) {
        *error = QStringLiteral("腾讯云返回了无法识别的内容");
        return {};
    }
    return response.value(QStringLiteral("TargetText")).toString();
}

} // namespace Translate

Translator::Translator(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

bool Translator::isConfigured()
{
    switch (Config::translateProvider()) {
    case Translate::Provider::Baidu:
        return !Config::baiduAppId().isEmpty() && !Config::translateSecret("baiduKey").isEmpty();
    case Translate::Provider::Tencent:
        return !Config::tencentSecretId().isEmpty() && !Config::translateSecret("tencentKey").isEmpty();
    case Translate::Provider::None:
        break;
    }
    return false;
}

QString Translator::providerName(Translate::Provider provider)
{
    switch (provider) {
    case Translate::Provider::Baidu:
        return tr("百度翻译");
    case Translate::Provider::Tencent:
        return tr("腾讯云机器翻译");
    case Translate::Provider::None:
        break;
    }
    return tr("不使用");
}

void Translator::translate(const QString &text)
{
    m_provider = Config::translateProvider();
    if (!isConfigured()) {
        emit finished({}, tr("请先在 设置 → 翻译 里选择服务并填写密钥。"));
        return;
    }
    m_pending = Translate::chunks(text.trimmed(), kChunkChars);
    m_done.clear();
    m_target = Translate::targetFor(text);
    if (m_pending.isEmpty()) {
        emit finished({}, tr("没有可翻译的文字。"));
        return;
    }
    sendNext();
}

void Translator::sendNext()
{
    const QString text = m_pending.first();
    QNetworkReply *reply = nullptr;
    if (m_provider == Translate::Provider::Baidu) {
        const QString salt = QString::number(QRandomGenerator::global()->bounded(10'000'000, 99'999'999));
        const QUrlQuery form =
            Translate::baiduForm(Config::baiduAppId(), Config::translateSecret("baiduKey"), text, m_target, salt);
        QNetworkRequest request(m_endpoint.isEmpty() ? kBaiduUrl : m_endpoint);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        reply = m_network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    } else {
        const QByteArray body = Translate::tencentBody(text, m_target);
        const qint64 timestamp = QDateTime::currentSecsSinceEpoch();
        QNetworkRequest request(m_endpoint.isEmpty() ? kTencentUrl : m_endpoint);
        request.setRawHeader("Authorization",
                             Translate::tencentAuthorization(Config::tencentSecretId(), Config::translateSecret("tencentKey"),
                                                             body, timestamp)
                                 .toUtf8());
        request.setRawHeader("Content-Type", kTencentContentType);
        request.setRawHeader("Host", kTencentHost);
        request.setRawHeader("X-TC-Action", "TextTranslate");
        request.setRawHeader("X-TC-Timestamp", QByteArray::number(timestamp));
        request.setRawHeader("X-TC-Version", "2018-03-21");
        request.setRawHeader("X-TC-Region", Config::tencentRegion().toUtf8());
        reply = m_network->post(request, body);
    }
    reply->setParent(this);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleReply(reply); });
}

void Translator::handleReply(QNetworkReply *reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();
    QString error;
    QString translated;
    if (reply->error() != QNetworkReply::NoError && data.isEmpty()) {
        error = tr("网络错误：%1").arg(reply->errorString());
    } else {
        translated = m_provider == Translate::Provider::Baidu ? Translate::parseBaidu(data, &error)
                                                              : Translate::parseTencent(data, &error);
    }
    if (!error.isEmpty()) {
        m_pending.clear();
        emit finished(m_done.join(QLatin1Char('\n')), error);
        return;
    }
    m_done << translated;
    m_pending.removeFirst();
    if (m_pending.isEmpty()) {
        emit finished(m_done.join(QLatin1Char('\n')), QString());
        return;
    }
    QTimer::singleShot(m_provider == Translate::Provider::Baidu ? kBaiduIntervalMs : 250, this, &Translator::sendNext);
}
