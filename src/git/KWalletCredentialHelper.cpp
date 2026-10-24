#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTextStream>

#include <memory>

namespace
{
using Attributes = QMap<QString, QString>;

class PromptSignalReceiver final : public QObject
{
    Q_OBJECT
public:
    bool dismissed = true;
    QEventLoop loop;

public slots:
    void completed(bool wasDismissed, const QDBusVariant &)
    {
        dismissed = wasDismissed;
        loop.quit();
    }
};

bool requestWalletCreation(const QString &wallet, QString *error)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface service(QStringLiteral("org.freedesktop.secrets"), QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"), bus);
    if (!service.isValid()) {
        if (error) *error = QStringLiteral("KWallet Secret Service is not available on the session bus.");
        return false;
    }
    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Collection.Label"), wallet);
    const QDBusMessage reply = service.call(QStringLiteral("CreateCollection"), properties, QString());
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() < 2) {
        if (error) *error = reply.errorMessage().isEmpty() ? QStringLiteral("KWallet did not start wallet creation.") : reply.errorMessage();
        return false;
    }
    const QDBusObjectPath collectionPath = reply.arguments().at(0).value<QDBusObjectPath>();
    const QDBusObjectPath promptPath = reply.arguments().at(1).value<QDBusObjectPath>();
    if (promptPath.path() == QLatin1String("/")) {
        const bool created = collectionPath.path() != QLatin1String("/");
        if (!created && error) *error = QStringLiteral("KWallet did not create the requested wallet.");
        return created;
    }

    PromptSignalReceiver receiver;
    if (!bus.connect(QStringLiteral("org.freedesktop.secrets"), promptPath.path(),
            QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"),
            &receiver, SLOT(completed(bool,QDBusVariant)))) {
        if (error) *error = QStringLiteral("Could not listen for KWallet’s wallet creation prompt.");
        return false;
    }
    QDBusInterface prompt(QStringLiteral("org.freedesktop.secrets"), promptPath.path(),
        QStringLiteral("org.freedesktop.Secret.Prompt"), bus);
    const QDBusMessage promptReply = prompt.call(QStringLiteral("Prompt"), QString());
    if (promptReply.type() == QDBusMessage::ErrorMessage) {
        bus.disconnect(QStringLiteral("org.freedesktop.secrets"), promptPath.path(),
            QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"),
            &receiver, SLOT(completed(bool,QDBusVariant)));
        if (error) *error = promptReply.errorMessage();
        return false;
    }
    receiver.loop.exec();
    bus.disconnect(QStringLiteral("org.freedesktop.secrets"), promptPath.path(),
        QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"),
        &receiver, SLOT(completed(bool,QDBusVariant)));
    if (receiver.dismissed && error) *error = QStringLiteral("Wallet creation was cancelled.");
    return !receiver.dismissed;
}

Attributes readAttributes()
{
    Attributes attributes;
    QTextStream input(stdin);
    while (!input.atEnd()) {
        const QString line = input.readLine();
        if (line.isEmpty()) break;
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals > 0) attributes.insert(line.left(equals), line.mid(equals + 1));
    }
    return attributes;
}

QString entryKey(const Attributes &attributes)
{
    const QByteArray identity = (attributes.value(QStringLiteral("protocol")) + QLatin1Char('\n')
        + attributes.value(QStringLiteral("host")) + QLatin1Char('\n')
        + attributes.value(QStringLiteral("path")) + QLatin1Char('\n')
        + attributes.value(QStringLiteral("username"))).toUtf8();
    return QStringLiteral("git-credential-%1").arg(QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()));
}

bool matches(const QJsonObject &credential, const Attributes &request)
{
    for (const QString &key : {QStringLiteral("protocol"), QStringLiteral("host"), QStringLiteral("path")}) {
        if (credential.value(key).toString() != request.value(key)) return false;
    }
    return request.value(QStringLiteral("username")).isEmpty()
        || credential.value(QStringLiteral("username")).toString() == request.value(QStringLiteral("username"));
}

class WalletClient
{
public:
    explicit WalletClient(const QString &walletName)
    {
        const QStringList services{QStringLiteral("org.kde.kwalletd6"), QStringLiteral("org.kde.kwalletd5")};
        for (const QString &service : services) {
            auto candidate = new QDBusInterface(service, QStringLiteral("/modules/kwalletd6"),
                QStringLiteral("org.kde.KWallet"), QDBusConnection::sessionBus());
            if (!candidate->isValid()) {
                delete candidate;
                candidate = new QDBusInterface(service, QStringLiteral("/modules/kwalletd5"),
                    QStringLiteral("org.kde.KWallet"), QDBusConnection::sessionBus());
            }
            if (candidate->isValid()) { interface = candidate; break; }
            delete candidate;
        }
        if (!interface) { error = QStringLiteral("KWallet is not available on the session bus."); return; }

        const QDBusReply<int> opened = interface->call(QStringLiteral("open"), walletName,
            qlonglong(0), QCoreApplication::applicationName());
        if (!opened.isValid() || opened.value() < 0) {
            error = opened.isValid() ? QStringLiteral("KWallet did not open the requested wallet.") : opened.error().message();
            return;
        }
        handle = opened.value();
    }

    ~WalletClient() { delete interface; }

    bool ready() const { return interface && handle >= 0; }
    QString error;

    bool selectFolder(const QString &folder)
    {
        const QString app = QCoreApplication::applicationName();
        const QDBusReply<bool> exists = interface->call(QStringLiteral("hasFolder"), handle, folder, app);
        if (!exists.isValid()) {
            error = exists.error().message();
            return false;
        }
        if (!exists.value()) {
            const QDBusReply<bool> created = interface->call(QStringLiteral("createFolder"), handle, folder, app);
            if (!created.isValid() || !created.value()) {
                error = created.isValid() ? QStringLiteral("KWallet could not create the selected folder.") : created.error().message();
                return false;
            }
        }
        const QDBusReply<bool> verified = interface->call(QStringLiteral("hasFolder"), handle, folder, app);
        if (!verified.isValid() || !verified.value()) {
            error = verified.isValid() ? QStringLiteral("KWallet could not select the selected folder.") : verified.error().message();
            return false;
        }
        selectedFolder = folder;
        return true;
    }

    QStringList entries()
    {
        const QDBusReply<QStringList> reply = interface->call(QStringLiteral("entryList"), handle,
            selectedFolder, QCoreApplication::applicationName());
        if (!reply.isValid()) { error = reply.error().message(); return {}; }
        return reply.value();
    }

    QString read(const QString &key)
    {
        const QDBusReply<QString> reply = interface->call(QStringLiteral("readPassword"), handle,
            selectedFolder, key, QCoreApplication::applicationName());
        if (!reply.isValid()) { error = reply.error().message(); return {}; }
        return reply.value();
    }

    bool write(const QString &key, const QString &value)
    {
        const QDBusReply<int> reply = interface->call(QStringLiteral("writePassword"), handle,
            selectedFolder, key, value, QCoreApplication::applicationName());
        if (!reply.isValid() || reply.value() != 0) {
            error = reply.isValid() ? QStringLiteral("KWallet could not save this credential.") : reply.error().message();
            return false;
        }
        return true;
    }

    bool remove(const QString &key)
    {
        const QDBusReply<int> reply = interface->call(QStringLiteral("removeEntry"), handle,
            selectedFolder, key, QCoreApplication::applicationName());
        if (!reply.isValid() || reply.value() != 0) {
            error = reply.isValid() ? QStringLiteral("KWallet could not remove this credential.") : reply.error().message();
            return false;
        }
        return true;
    }

private:
    QDBusInterface *interface = nullptr;
    int handle = -1;
    QString selectedFolder;
};

int runHelper(const QStringList &arguments)
{
    QString wallet = QStringLiteral("git-credentials");
    QString folder = QStringLiteral("Git Credentials");
    QString operation;
    bool createWallet = false;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i) == QStringLiteral("--wallet") && i + 1 < arguments.size()) wallet = arguments.at(++i);
        else if (arguments.at(i) == QStringLiteral("--folder") && i + 1 < arguments.size()) folder = arguments.at(++i);
        else if (arguments.at(i) == QStringLiteral("--create-wallet")) createWallet = true;
        else operation = arguments.at(i);
    }
    if (wallet.isEmpty() || folder.isEmpty()) {
        QTextStream(stderr) << "Wallet and folder names cannot be empty.\n";
        return 1;
    }

    auto client = std::make_unique<WalletClient>(wallet);
    if (createWallet && !client->ready()) {
        QString creationError;
        if (!requestWalletCreation(wallet, &creationError)) {
            QTextStream(stderr) << creationError << Qt::endl;
            return 1;
        }
        client = std::make_unique<WalletClient>(wallet);
    }
    if (!client->ready()) { QTextStream(stderr) << client->error << Qt::endl; return createWallet ? 1 : 0; }
    if (!client->selectFolder(folder)) { QTextStream(stderr) << client->error << Qt::endl; return 1; }
    if (createWallet) return 0;

    const Attributes request = readAttributes();
    if (operation == QStringLiteral("store")) {
        const QJsonObject credential{{QStringLiteral("protocol"), request.value(QStringLiteral("protocol"))},
            {QStringLiteral("host"), request.value(QStringLiteral("host"))},
            {QStringLiteral("path"), request.value(QStringLiteral("path"))},
            {QStringLiteral("username"), request.value(QStringLiteral("username"))},
            {QStringLiteral("password"), request.value(QStringLiteral("password"))}};
        if (credential.value(QStringLiteral("host")).toString().isEmpty()
            || credential.value(QStringLiteral("password")).toString().isEmpty()) return 0;
        return client->write(entryKey(request), QString::fromUtf8(QJsonDocument(credential).toJson(QJsonDocument::Compact))) ? 0 : 1;
    }
    if (operation == QStringLiteral("get")) {
        for (const QString &key : client->entries()) {
            if (!key.startsWith(QStringLiteral("git-credential-"))) continue;
            const QJsonDocument document = QJsonDocument::fromJson(client->read(key).toUtf8());
            if (!document.isObject() || !matches(document.object(), request)) continue;
            const QJsonObject credential = document.object();
            QTextStream output(stdout);
            output << "protocol=" << credential.value(QStringLiteral("protocol")).toString() << '\n'
                   << "host=" << credential.value(QStringLiteral("host")).toString() << '\n';
            const QString path = credential.value(QStringLiteral("path")).toString();
            const QString username = credential.value(QStringLiteral("username")).toString();
            if (!path.isEmpty()) output << "path=" << path << '\n';
            if (!username.isEmpty()) output << "username=" << username << '\n';
            output << "password=" << credential.value(QStringLiteral("password")).toString() << "\n\n";
            break;
        }
        return 0;
    }
    if (operation == QStringLiteral("erase")) {
        bool success = true;
        const QString exactKey = entryKey(request);
        const QStringList keys = request.value(QStringLiteral("username")).isEmpty() ? client->entries() : QStringList{exactKey};
        for (const QString &key : keys) {
            if (!key.startsWith(QStringLiteral("git-credential-"))) continue;
            if (key == exactKey || matches(QJsonDocument::fromJson(client->read(key).toUtf8()).object(), request))
                success = client->remove(key) && success;
        }
        if (!success) QTextStream(stderr) << client->error << Qt::endl;
    }
    return 0;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("shell-scratchpad-git"));
    return runHelper(QCoreApplication::arguments());
}

#include "KWalletCredentialHelper.moc"
