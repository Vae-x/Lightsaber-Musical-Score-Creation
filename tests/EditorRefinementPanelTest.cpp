#include "gui/AiRecognitionPage.h"
#include "gui/EditorRefinementPanel.h"
#include "gui/TaskProgressView.h"
#include "gui/ThemeManager.h"

#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
QRect widgetRect(const QWidget *widget, QWidget *ancestor) {
    return QRect(widget->mapTo(ancestor, QPoint()), widget->size());
}
void verifyProgressGeometry(lmsc::TaskProgressView *task, QScrollArea *details, QWidget *panel) {
    const auto stage = task->stageLabel();
    QVERIFY(stage->isVisible());
    QVERIFY2(stage->height() >= stage->heightForWidth(stage->width()), "Wrapped task stage was compressed below its text height");
    QVERIFY(task->height() >= task->heightForWidth(task->width()));
    QVERIFY(task->rect().contains(widgetRect(stage, task)));
    QVERIFY(task->rect().contains(widgetRect(task->progressBar(), task)));
    QVERIFY(!widgetRect(stage, task).intersects(widgetRect(task->progressBar(), task)));
    if (details->isVisible()) {
        const auto detailsRect = widgetRect(details, panel), taskRect = widgetRect(task, panel);
        QVERIFY2(detailsRect.bottom() < taskRect.top(), "Details overlapped the task state");
        QVERIFY(!detailsRect.intersects(widgetRect(stage, panel)));
    }
    QCOMPARE(task->progressBar()->maximumHeight(), QWIDGETSIZE_MAX);
}
double luminance(const QColor &color) {
    const auto channel = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
    return .2126 * channel(color.redF()) + .7152 * channel(color.greenF()) + .0722 * channel(color.blueF());
}
}

class EditorRefinementPanelTest : public QObject {
    Q_OBJECT
private slots:
    void rangeAndVersionIntents();
    void taskStateNeverScrollsWithDetails_data();
    void taskStateNeverScrollsWithDetails();
    void compactGenerationKeepsTaskVisible();
    void stackedCompactPanelsReserveWrappedStage_data();
    void stackedCompactPanelsReserveWrappedStage();
    void generationToggleContrast_data();
    void generationToggleContrast();
};

void EditorRefinementPanelTest::rangeAndVersionIntents() {
    lmsc::EditorRefinementPanel panel;
    lmsc::EditorRefinementPanel::State state;
    state.available = state.sourceValid = state.hasCandidate = state.canRefine = state.canApply = true;
    state.durationSeconds = 40;
    state.hasRefinement = true;
    QSignalSpy versions(&panel, &lmsc::EditorRefinementPanel::versionRequested);
    QSignalSpy refine(&panel, &lmsc::EditorRefinementPanel::refineRequested);
    QSignalSpy cancel(&panel, &lmsc::EditorRefinementPanel::cancelRequested);
    panel.setState(state);
    QCOMPARE(versions.count(), 0);
    auto version = panel.findChild<QComboBox *>(QStringLiteral("editorCandidateVersion"));
    QCOMPARE(version->count(), 4);
    version->setCurrentIndex(version->findData(lmsc::EditorRefinementPanel::Initial));
    QCOMPARE(versions.count(), 1);
    QCOMPARE(versions.takeFirst().at(0).value<lmsc::EditorRefinementPanel::Version>(), lmsc::EditorRefinementPanel::Initial);

    auto refineButton = panel.findChild<QPushButton *>(QStringLiteral("editorRefineButton"));
    panel.setSelection(4, 2);
    QVERIFY(!refineButton->isEnabled());
    panel.setSelection(1, 6);
    QVERIFY(refineButton->isEnabled());
    refineButton->click();
    QCOMPARE(refine.count(), 1);
    const auto args = refine.takeFirst();
    QCOMPARE(args[0].toBool(), true);
    QCOMPARE(args[1].toDouble(), 1.0);
    QCOMPARE(args[2].toDouble(), 6.0);

    panel.setSelection(2, 3, false);
    refineButton->click();
    const auto full = refine.takeFirst();
    QCOMPARE(full[0].toBool(), false);
    QCOMPARE(full[1].toDouble(), 0.0);
    QCOMPARE(full[2].toDouble(), 40.0);
    state.running = true;
    panel.setState(state);
    QVERIFY(!refineButton->isEnabled());
    QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("editorCandidateApply"))->isEnabled());
    auto cancelButton = panel.findChild<QPushButton *>(QStringLiteral("editorRefinementCancel"));
    QVERIFY(cancelButton->isEnabled());
    cancelButton->click();
    QCOMPARE(cancel.count(), 1);
}

void EditorRefinementPanelTest::taskStateNeverScrollsWithDetails_data() {
    QTest::addColumn<QString>("theme");
    QTest::addColumn<int>("fontPixels");
    QTest::newRow("light-normal") << QStringLiteral("light") << 13;
    QTest::newRow("dark-large") << QStringLiteral("dark") << 20;
}

void EditorRefinementPanelTest::taskStateNeverScrollsWithDetails() {
    QFETCH(QString, theme);
    QFETCH(int, fontPixels);
    const auto original = lmsc::ThemeManager::mode();
    lmsc::ThemeManager::apply(theme);
    lmsc::EditorRefinementPanel panel;
    panel.setStyleSheet(QStringLiteral("QWidget { font-size: %1px; }").arg(fontPixels));
    lmsc::EditorRefinementPanel::State state;
    state.available = state.sourceValid = state.hasCandidate = state.canRefine = state.running = true;
    state.durationSeconds = 240;
    panel.setState(state);
    lmsc::RefinementResult result;
    for (int i = 0; i < 60; ++i) result.processedRanges.append(QStringLiteral("p%1 · 2.000–8.000 秒").arg(i));
    panel.setResult(result);
    panel.setProgress(31, QStringLiteral("精修乐句 2/5 · 剩余 8 个乐句"));
    panel.resize(360, 560);
    panel.show();
    QTest::qWait(20);
    const auto task = panel.taskProgress();
    auto percentage = task->percentageLabel();
    QCOMPARE(percentage->text(), QStringLiteral("31%"));
    QVERIFY(percentage->isVisible());
    QVERIFY(!task->progressBar()->isTextVisible());
    for (auto ancestor = task->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        QVERIFY(!qobject_cast<QScrollArea *>(ancestor));
    const QRect percentRect(percentage->mapTo(&panel, QPoint()), percentage->size());
    QVERIFY(panel.rect().contains(percentRect));
    QVERIFY(percentage->height() >= percentage->fontMetrics().height());
    auto scroll = panel.findChild<QScrollArea *>(QStringLiteral("editorRefinementDetailsScroll"));
    verifyProgressGeometry(task, scroll, &panel);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCOMPARE(percentage->text(), QStringLiteral("31%"));
    QVERIFY(panel.rect().contains(QRect(percentage->mapTo(&panel, QPoint()), percentage->size())));
    panel.setProgress(100, QStringLiteral("本批完成，可继续剩余乐句"));
    QCOMPARE(percentage->text(), QStringLiteral("100%"));
    panel.setProgress(-1, QStringLiteral("正在建立音乐上下文"));
    QCOMPARE(task->progressBar()->maximum(), 0);
    QCOMPARE(percentage->text(), QStringLiteral("处理中"));
    verifyProgressGeometry(task, scroll, &panel);
    lmsc::ThemeManager::apply(original);
}

void EditorRefinementPanelTest::stackedCompactPanelsReserveWrappedStage_data() {
    QTest::addColumn<QString>("theme");
    QTest::addColumn<int>("fontPixels");
    QTest::newRow("light-normal") << QStringLiteral("light") << 13;
    QTest::newRow("dark-normal") << QStringLiteral("dark") << 13;
    QTest::newRow("light-large") << QStringLiteral("light") << 20;
    QTest::newRow("dark-large") << QStringLiteral("dark") << 20;
}

void EditorRefinementPanelTest::stackedCompactPanelsReserveWrappedStage() {
    QFETCH(QString, theme); QFETCH(int, fontPixels);
    const auto original = lmsc::ThemeManager::mode(); lmsc::ThemeManager::apply(theme);
    QWidget column;
    column.setStyleSheet(QStringLiteral("QWidget { font-size: %1px; }").arg(fontPixels));
    auto layout = new QVBoxLayout(&column);
    lmsc::AiRecognitionPage generation;
    generation.setCompact(true); generation.setExpanded(true);
    generation.taskProgress()->setProgress(31, QStringLiteral("AI 整曲建议已经完成，正在根据重音和动作主题进行本地编排"));
    lmsc::EditorRefinementPanel refinement;
    lmsc::EditorRefinementPanel::State state;
    state.hasCandidate = state.available = state.sourceValid = state.running = true;
    state.summary = QStringLiteral("由正式谱建立的精修草稿，确认应用前正式谱保持原状。\n未应用草稿不会导出。保存工程会同时保存工作草稿和对比版本。");
    refinement.setState(state);
    refinement.setProgress(31, QStringLiteral("精修乐句 2/5 · 9.3–11.1 秒 · 剩余 8 个乐句，正在核查相邻连接和完整谱面"));
    layout->addWidget(&generation, 1); layout->addWidget(&refinement);
    column.resize(290, 550); column.show(); QTest::qWait(30);
    auto generationScroll = generation.findChild<QScrollArea *>(QStringLiteral("aiGenerationDetailsScroll"));
    auto refinementScroll = refinement.findChild<QScrollArea *>(QStringLiteral("editorRefinementDetailsScroll"));
    verifyProgressGeometry(generation.taskProgress(), generationScroll, &generation);
    verifyProgressGeometry(refinement.taskProgress(), refinementScroll, &refinement);
    const int longHeight = refinement.taskProgress()->minimumHeight();
    refinement.setProgress(31, QStringLiteral("核查中")); QTest::qWait(20);
    QVERIFY(refinement.taskProgress()->minimumHeight() < longHeight);
    verifyProgressGeometry(refinement.taskProgress(), refinementScroll, &refinement);
    column.setStyleSheet(QStringLiteral("QWidget { font-size: %1px; }").arg(fontPixels + 6));
    QTest::qWait(20);
    verifyProgressGeometry(generation.taskProgress(), generationScroll, &generation);
    verifyProgressGeometry(refinement.taskProgress(), refinementScroll, &refinement);
    lmsc::ThemeManager::apply(original);
}

void EditorRefinementPanelTest::generationToggleContrast_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QStringLiteral("light");
    QTest::newRow("dark") << QStringLiteral("dark");
}

void EditorRefinementPanelTest::generationToggleContrast() {
    QFETCH(QString, theme);
    const auto original = lmsc::ThemeManager::mode(); lmsc::ThemeManager::apply(theme);
    lmsc::AiRecognitionPage panel; panel.setCompact(true); panel.setExpanded(true);
    panel.resize(320, 450); panel.show(); QTest::qWait(20);
    auto toggle = panel.findChild<QToolButton *>(QStringLiteral("aiGenerationPanelToggle"));
    for (bool expanded : {true, false}) {
        panel.setExpanded(expanded); QTest::qWait(20);
        const auto image = toggle->grab().toImage();
        const QColor background = image.pixelColor(image.width() - 12, image.height() / 2);
        // Qt's widget palette does not expose colors from the :checked paint
        // rule. Verify that the expected theme color actually paints the glyphs.
        const QColor foreground = qApp->palette().color(expanded ? QPalette::HighlightedText : QPalette::ButtonText);
        int textPixels = 0, arrowPixels = 0;
        const int arrowEnd = qRound(32 * image.devicePixelRatio());
        for (int y = 3; y < image.height() - 3; ++y)
            for (int x = 5; x < image.width() - 12; ++x) {
                const QColor pixel = image.pixelColor(x,y);
                if (qAbs(pixel.red() - foreground.red()) <= 2 && qAbs(pixel.green() - foreground.green()) <= 2
                        && qAbs(pixel.blue() - foreground.blue()) <= 2) {
                    if (x < arrowEnd) ++arrowPixels; else ++textPixels;
                }
            }
        QVERIFY2(textPixels >= 12, "Toggle text did not paint in its readable theme color");
        QVERIFY2(arrowPixels >= 4, "Toggle arrow did not paint in its readable theme color");
        const auto a = luminance(background), b = luminance(foreground);
        const auto contrast = (qMax(a,b) + .05) / (qMin(a,b) + .05);
        QVERIFY2(contrast >= 4.5, qPrintable(QStringLiteral("Toggle expanded=%1, text=%2, background=%3, contrast=%4")
            .arg(expanded).arg(foreground.name()).arg(background.name()).arg(contrast)));
        QCOMPARE(toggle->arrowType(), expanded ? Qt::DownArrow : Qt::RightArrow);
    }
    lmsc::ThemeManager::apply(original);
}

void EditorRefinementPanelTest::compactGenerationKeepsTaskVisible() {
    lmsc::AiRecognitionPage panel;
    panel.setCompact(true);
    QVERIFY(!panel.isExpanded());
    panel.taskProgress()->setProgress(31, QStringLiteral("AI 建议已完成，正在本地编排"));
    panel.resize(360, 450);
    panel.show();
    QTest::qWait(20);
    auto details = panel.findChild<QScrollArea *>(QStringLiteral("aiGenerationDetailsScroll"));
    QVERIFY(!details->isVisible());
    auto percentage = panel.taskProgress()->percentageLabel();
    QVERIFY(percentage->isVisible());
    QCOMPARE(percentage->text(), QStringLiteral("31%"));
    panel.findChild<QToolButton *>(QStringLiteral("aiGenerationPanelToggle"))->click();
    QVERIFY(panel.isExpanded());
    QVERIFY(details->isVisible());
    QVERIFY(percentage->isVisible());
    QCOMPARE(panel.findChild<QComboBox *>(QStringLiteral("aiGenerationMode"))->count(), 3);
    auto progress = panel.findChild<QProgressBar *>(QStringLiteral("aiRecognitionProgress"));
    QCOMPARE(progress, panel.taskProgress()->progressBar());
    for (auto ancestor = progress->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        QVERIFY(!qobject_cast<QScrollArea *>(ancestor));
}

QTEST_MAIN(EditorRefinementPanelTest)
#include "EditorRefinementPanelTest.moc"
