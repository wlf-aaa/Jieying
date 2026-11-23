#include "capturehistory.h"
#include "collagedialog.h"
#include "config.h"
#include "historydialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {

QImage screenshot(QColor color, QSize size = QSize(160, 120))
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

} // namespace

class HistoryDialogTest : public QObject
{
    Q_OBJECT

private slots:
    void listsNewestFirstAndCopiesKeyboardSelection();
    void previewsAtAvailableSizeAndPinsOriginal();
    void showsVeryThinCaptures();
    void refreshesOnWriteAndKeepsTheSelection();
    void appendsSelectedCapturesToCollageInListOrder();
    void disablesActionsForMissingAndUnreadableImages();
    void alwaysPromptsForSaveAs();
};

void HistoryDialogTest::listsNewestFirstAndCopiesKeyboardSelection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(screenshot(Qt::red).save(dir.filePath("20260401_094501_100.png")));
    QVERIFY(screenshot(Qt::blue).save(dir.filePath("20260401_094502_100.png")));

    CaptureHistory history(dir.path());
    HistoryDialog dialog(&history);
    QVERIFY(!dialog.isModal());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowActive(&dialog));
    auto *list = dialog.findChild<QListWidget *>("historyList");
    QVERIFY(list);
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->currentRow(), 0);
    QVERIFY(!list->item(0)->icon().isNull());
    QVERIFY(!list->item(1)->icon().isNull());

    const auto copy = [list] {
        QApplication::clipboard()->clear();
        QTest::keyClick(list, Qt::Key_C, Qt::ControlModifier);
        const QImage image = QApplication::clipboard()->image();
        return image.isNull() ? QColor() : image.pixelColor(0, 0);
    };
    QCOMPARE(copy(), QColor(Qt::blue));
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(copy(), QColor(Qt::red));
    QTest::keyClick(list, Qt::Key_Up);
    QCOMPARE(copy(), QColor(Qt::blue));
}

void HistoryDialogTest::previewsAtAvailableSizeAndPinsOriginal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QSize sourceSize(1600, 400);
    QVERIFY(screenshot(Qt::magenta, sourceSize).save(dir.filePath("20260401_094501_100.png")));

    CaptureHistory history(dir.path());
    HistoryDialog dialog(&history);
    dialog.resize(880, 560);
    dialog.show();
    auto *preview = dialog.findChild<QWidget *>("historyPreview");
    auto *pin = dialog.findChild<QPushButton *>("historyPin");
    QVERIFY(preview);
    QVERIFY(pin);
    QVERIFY(preview->width() > 400);

    QImage rendered = preview->grab().toImage();
    const auto middle = [&rendered] { return rendered.rect().center(); };
    QCOMPARE(rendered.pixelColor(middle()), QColor(Qt::magenta));
    QCOMPARE(rendered.pixelColor(rendered.width() / 8, middle().y()), QColor(Qt::magenta));
    QVERIFY(rendered.pixelColor(middle().x(), rendered.height() / 8) != QColor(Qt::magenta));
    const int originalWidth = preview->width();
    dialog.resize(760, 560);
    QTRY_VERIFY(preview->width() < originalWidth);
    rendered = preview->grab().toImage();
    QCOMPARE(rendered.pixelColor(middle()), QColor(Qt::magenta));
    QCOMPARE(rendered.pixelColor(rendered.width() / 8, middle().y()), QColor(Qt::magenta));

    QSignalSpy pinned(&dialog, &HistoryDialog::pinRequested);
    pin->click();
    QCOMPARE(pinned.count(), 1);
    const auto image = qvariant_cast<QPixmap>(pinned.first().first());
    QCOMPARE(image.size(), sourceSize);
    QCOMPARE(image.toImage().pixelColor(0, 0), QColor(Qt::magenta));
}

void HistoryDialogTest::showsVeryThinCaptures()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(screenshot(Qt::magenta, QSize(8000, 4)).save(dir.filePath("20260401_094501_100.png")));

    CaptureHistory history(dir.path());
    HistoryDialog dialog(&history);
    dialog.show();
    auto *list = dialog.findChild<QListWidget *>("historyList");
    auto *preview = dialog.findChild<QWidget *>("historyPreview");
    QVERIFY(list);
    QVERIFY(preview);
    QVERIFY(!list->item(0)->icon().isNull());
    const QImage rendered = preview->grab().toImage();
    QCOMPARE(rendered.pixelColor(rendered.rect().center()), QColor(Qt::magenta));
}

void HistoryDialogTest::refreshesOnWriteAndKeepsTheSelection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Config::setHistorySize(20);
    QVERIFY(screenshot(Qt::red).save(dir.filePath("20000101_120001_100.png")));
    QVERIFY(screenshot(Qt::blue).save(dir.filePath("20000101_120002_100.png")));

    CaptureHistory history(dir.path());
    HistoryDialog dialog(&history);
    auto *list = dialog.findChild<QListWidget *>("historyList");
    auto *pin = dialog.findChild<QPushButton *>("historyPin");
    QVERIFY(list);
    QVERIFY(pin);
    list->setCurrentRow(1);
    history.add(screenshot(Qt::green));
    QTRY_COMPARE(list->count(), 3);
    QCOMPARE(list->currentRow(), 2);

    QSignalSpy pinned(&dialog, &HistoryDialog::pinRequested);
    pin->click();
    QCOMPARE(pinned.count(), 1);
    const auto image = qvariant_cast<QPixmap>(pinned.first().first());
    QCOMPARE(image.toImage().pixelColor(0, 0), QColor(Qt::red));

    history.clear();
    QCOMPARE(list->count(), 0);
    QVERIFY(!pin->isEnabled());
}

void HistoryDialogTest::appendsSelectedCapturesToCollageInListOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir("history"));
    const QString older = dir.filePath("history/20000101_120001_100.png");
    const QString middle = dir.filePath("history/20000101_120002_100.png");
    const QString newer = dir.filePath("history/20000101_120003_100.png");
    const QString existing = dir.filePath("existing.png");
    QVERIFY(screenshot(Qt::red).save(older));
    QVERIFY(screenshot(Qt::green).save(middle));
    QVERIFY(screenshot(Qt::blue).save(newer));
    QVERIFY(screenshot(Qt::black).save(existing));

    CaptureHistory history(dir.filePath("history"));
    HistoryDialog dialog(&history);
    CollageDialog collage;
    collage.addImages({existing});
    connect(&dialog, &HistoryDialog::collageRequested, &collage, &CollageDialog::addImages);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowActive(&dialog));
    auto *historyList = dialog.findChild<QListWidget *>("historyList");
    auto *collageList = collage.findChild<QListWidget *>();
    auto *addToCollage = dialog.findChild<QPushButton *>("historyCollage");
    QVERIFY(historyList && collageList && addToCollage);
    QVERIFY(!addToCollage->isEnabled());

    QTest::mouseClick(historyList->viewport(), Qt::LeftButton, {},
                      historyList->visualItemRect(historyList->item(2)).center());
    QTest::mouseClick(historyList->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      historyList->visualItemRect(historyList->item(0)).center());
    QCOMPARE(historyList->selectedItems().size(), 2);
    QVERIFY(addToCollage->isEnabled());

    history.add(screenshot(Qt::yellow));
    QTRY_COMPARE(historyList->count(), 4);
    QCOMPARE(historyList->selectedItems().size(), 2);
    QCOMPARE(historyList->currentRow(), 1);

    addToCollage->click();
    QCOMPARE(collageList->count(), 3);
    QCOMPARE(collageList->item(0)->text(), QFileInfo(existing).fileName());
    QCOMPARE(collageList->item(1)->text(), QFileInfo(newer).fileName());
    QCOMPARE(collageList->item(2)->text(), QFileInfo(older).fileName());

    QVERIFY(QFile::remove(newer));
    emit history.changed();
    QCOMPARE(historyList->selectedItems().size(), 1);
    QVERIFY(!addToCollage->isEnabled());
}

void HistoryDialogTest::disablesActionsForMissingAndUnreadableImages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString validPath = dir.filePath("20260401_094501_100.png");
    const QString brokenPath = dir.filePath("20260401_094502_100.png");
    QVERIFY(screenshot(Qt::red).save(validPath));
    QFile badImage(brokenPath);
    QVERIFY(badImage.open(QIODevice::WriteOnly));
    QVERIFY(badImage.write("invalid PNG") > 0);
    badImage.close();

    CaptureHistory history(dir.path());
    HistoryDialog dialog(&history);
    auto *list = dialog.findChild<QListWidget *>("historyList");
    auto *pin = dialog.findChild<QPushButton *>("historyPin");
    QVERIFY(list);
    QVERIFY(pin);
    QCOMPARE(list->currentRow(), 0);
    QVERIFY(!pin->isEnabled());
    QVERIFY(list->item(0)->icon().isNull());

    list->setCurrentRow(1);
    QVERIFY(pin->isEnabled());
    QSignalSpy pinned(&dialog, &HistoryDialog::pinRequested);
    QVERIFY(QFile::remove(validPath));
    pin->click();
    QCOMPARE(pinned.count(), 0);
    QVERIFY(!pin->isEnabled());

    QVERIFY(screenshot(Qt::blue).save(brokenPath));
    emit history.changed();
    QCOMPARE(list->count(), 1);
    QVERIFY(pin->isEnabled());
    QVERIFY(!list->item(0)->icon().isNull());
}

void HistoryDialogTest::alwaysPromptsForSaveAs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir("history"));
    const QString source = dir.filePath("history/20260401_094501_100.png");
    const QString target = dir.filePath("export.png");
    QVERIFY(screenshot(Qt::blue).save(source));
    Config::setSaveDir(dir.path());
    Config::setImageFormat("png");
    Config::setAskWhereToSave(false);

    CaptureHistory history(dir.filePath("history"));
    HistoryDialog dialog(&history);
    auto *save = dialog.findChild<QPushButton *>("historySaveAs");
    QVERIFY(save);
    QVERIFY(save->isEnabled());
    dialog.show();

    bool prompted = false;
    QTimer findDialog;
    connect(&findDialog, &QTimer::timeout, &dialog, [&] {
        auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (!picker)
            return;
        prompted = true;
        findDialog.stop();
        picker->selectFile(target);
        QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &dialog, [] {
        if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            modal->reject();
    });
    findDialog.start(20);
    timeout.start(3000);
    save->click();

    QVERIFY(prompted);
    const QImage exported(target);
    QVERIFY(!exported.isNull());
    QCOMPARE(exported.size(), QSize(160, 120));
    QCOMPARE(exported.pixelColor(0, 0), QColor(Qt::blue));
    QVERIFY(QFile::exists(source));
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_PLUGIN_PATH", QByteArray(JIEYING_QT_PLUGIN_DIR));
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);

    QTemporaryDir settings;
    if (!settings.isValid())
        return 1;
    QCoreApplication::setOrganizationName("JieyingTests");
    QCoreApplication::setApplicationName("HistoryDialogTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    HistoryDialogTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "historydialog_test.moc"
