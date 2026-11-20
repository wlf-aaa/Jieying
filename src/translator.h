#pragma once

#include "config.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

class QNetworkAccessManager;
class QNetworkReply;

// Online translation of recognized text with the user's own key. Nothing is sent unless the
// user asks for a translation; the provider and keys come from the settings.
namespace Translate {

using Provider = Config::TranslateProvider;

// Chinese text goes to English, anything else to Chinese ("zh" / "en").
QString targetFor(const QString &text);

// Pieces of at most maxChars, cut at line ends (a longer single line is cut where it must).
QStringList chunks(const QString &text, int maxChars);

// Baidu Fanyi general translation: form fields, sign = MD5(appid + q + salt + key).
QUrlQuery baiduForm(const QString &appId, const QString &key, const QString &text, const QString &to,
                    const QString &salt);
QString parseBaidu(const QByteArray &json, QString *error);

// Tencent Cloud TMT TextTranslate, signed with TC3-HMAC-SHA256.
QByteArray tencentBody(const QString &text, const QString &to);
QString tencentAuthorization(const QString &secretId, const QString &secretKey, const QByteArray &body,
                             qint64 timestamp);
QString parseTencent(const QByteArray &json, QString *error);

} // namespace Translate

// Translates text chunk by chunk with the configured provider; finished() fires once.
class Translator : public QObject
{
    Q_OBJECT

public:
    explicit Translator(QObject *parent = nullptr);

    // Whether the provider has what it needs (keys filled in).
    static bool isConfigured();
    static QString providerName(Translate::Provider provider);

    void translate(const QString &text);
    // Tests point the requests at a local server instead of the provider.
    void setEndpoint(const QUrl &url) { m_endpoint = url; }

signals:
    void finished(const QString &translation, const QString &error);

private:
    void sendNext();
    void handleReply(QNetworkReply *reply);

    QNetworkAccessManager *m_network;
    QUrl m_endpoint;
    Translate::Provider m_provider = Translate::Provider::None;
    QStringList m_pending;
    QStringList m_done;
    QString m_target;
};
