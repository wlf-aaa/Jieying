#include "ocrwindow.h"
#include "tableocr.h"
#include "tableocrdialog.h"
#ifdef JIEYING_HAVE_PADDLEOCR
#include "paddleocr.h"
#endif

#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QtConcurrent>

namespace {

QImage borderedTable(bool withText)
{
    QImage image(570, 300, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(QColor(196, 196, 196));
    for (const int x : {30, 220, 385, 539})
        painter.drawLine(x, 24, x, 275);
    for (const int y : {24, 98, 180, 275})
        painter.drawLine(30, y, 539, y);

    if (withText) {
        QFont font(QStringLiteral("Arial"));
        font.setPixelSize(32);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(46, 71, QStringLiteral("ALPHA"));
        painter.drawText(237, 71, QStringLiteral("BETA"));
        painter.drawText(402, 71, QStringLiteral("GAMMA"));
        painter.drawText(46, 145, QStringLiteral("120"));
        painter.drawText(402, 145, QStringLiteral("360"));
        painter.drawText(237, 221, QStringLiteral("NORTH"));
        painter.drawText(237, 255, QStringLiteral("SOUTH"));
    }
    return image;
}

TableOcr::Result recognize(const QImage &image, Config::OcrEngine engine = Config::OcrEngine::Windows)
{
    return QtConcurrent::run([=] { return TableOcr::recognize(image, engine); }).result();
}

} // namespace

class TableOcrTest : public QObject
{
    Q_OBJECT

private slots:
    void borderedTableKeepsBlankCellsAndMultilineContent_data();
    void borderedTableKeepsBlankCellsAndMultilineContent();
    void tableBordersMayTouchTheImageEdge();
    void irregularTableFallsBackToTextLines();
    void dataLimitsReturnAnError();
    void editedPreviewCopiesTheGridToClipboard();
};

void TableOcrTest::borderedTableKeepsBlankCellsAndMultilineContent_data()
{
    QTest::addColumn<int>("engine");
    QTest::newRow("windows") << int(Config::OcrEngine::Windows);
#ifdef JIEYING_HAVE_PADDLEOCR
    if (PaddleOcr::isAvailable())
        QTest::newRow("paddle") << int(Config::OcrEngine::Paddle);
#endif
}

void TableOcrTest::borderedTableKeepsBlankCellsAndMultilineContent()
{
    QFETCH(int, engine);
    const auto result = recognize(borderedTable(true), Config::OcrEngine(engine));
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.hasGrid);
    const QList<QStringList> expected = {
        {QStringLiteral("ALPHA"), QStringLiteral("BETA"), QStringLiteral("GAMMA")},
        {QStringLiteral("120"), QString(), QStringLiteral("360")},
        {QString(), QStringLiteral("NORTH\nSOUTH"), QString()},
    };
    QCOMPARE(result.cells, expected);
    QCOMPARE(TableOcr::toTsv(result.cells), QStringLiteral("ALPHA\tBETA\tGAMMA\n120\t\t360\n\tNORTH SOUTH\t"));
}

void TableOcrTest::irregularTableFallsBackToTextLines()
{
    QImage image = borderedTable(true);
    QPainter painter(&image);
    painter.fillRect(QRect(211, 26, 15, 70), Qt::white);
    painter.end();
    const auto result = recognize(image);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!result.hasGrid);
    QVERIFY(!result.cells.isEmpty());
    for (const auto &row : result.cells)
        QCOMPARE(row.size(), 1);
}

void TableOcrTest::tableBordersMayTouchTheImageEdge()
{
    QImage image = borderedTable(false).copy(30, 24, 510, 252);
    image.invertPixels();
    const auto result = recognize(image);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.hasGrid);
    QCOMPARE(result.cells, QList<QStringList>(3, QStringList(3)));
}

void TableOcrTest::dataLimitsReturnAnError()
{
    const QImage image(1, 16000, QImage::Format_RGB32);
    QVERIFY(!recognize(image).error.isEmpty());
    QVERIFY(!recognize({}).error.isEmpty());

    QImage grid(810, 810, QImage::Format_RGB32);
    grid.fill(Qt::white);
    QPainter painter(&grid);
    for (int i = 0; i <= 33; ++i) {
        const int edge = 10 + i * 24;
        painter.drawLine(edge, 10, edge, 802);
        painter.drawLine(10, edge, 802, edge);
    }
    painter.end();
    QVERIFY(!recognize(grid).error.isEmpty());
}

void TableOcrTest::editedPreviewCopiesTheGridToClipboard()
{
    auto *window = new OcrWindow(borderedTable(false));
    window->show();
    auto *entry = window->findChild<QPushButton *>(QStringLiteral("recognizeTable"));
    QVERIFY(entry);
    QVERIFY(entry->isVisible());
    QVERIFY(entry->isEnabled());
    entry->click();

    auto *dialog = window->findChild<TableOcrDialog *>();
    QVERIFY(dialog);
    auto *table = dialog->findChild<QTableWidget *>();
    auto *copy = dialog->findChild<QPushButton *>(QStringLiteral("copyTable"));
    QVERIFY(table);
    QVERIFY(copy);
    QTRY_COMPARE(table->rowCount(), 3);
    QCOMPARE(table->columnCount(), 3);
    QVERIFY(!copy->isEnabled());
    table->setCurrentCell(0, 0);
    QTest::keyClick(table, Qt::Key_F2);
    auto *editor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("hello"));
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_VERIFY(copy->isEnabled());
    table->setItem(0, 2, new QTableWidgetItem(QStringLiteral("a\tb\r\nc")));
    copy->click();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello\t\ta b c\n\t\t\n\t\t"));
    window->close();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    qputenv("QT_PLUGIN_PATH", JIEYING_QT_PLUGIN_DIR);
    QApplication app(argc, argv);
    TableOcrTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tableocr_test.moc"
