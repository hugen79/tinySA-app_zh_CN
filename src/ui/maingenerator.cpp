// MainWindow: the signal generator page and the switch between analyzer and generator.
// The tinySA is one or the other, so the generator page replaces the analyzer's controls
// and graph, as the unit's own screen does in output mode.
#include "mainwindow.h"
#include "generatorpanel.h"
#include "ui_mainwindow.h"
#include <QLabel>
#include <QShortcut>

void MainWindow::setupGenerator()
{
    connect(ui->generatorPanel, &GeneratorPanel::edited, &m_session, &Session::setGenerator);
    connect(ui->generatorPanel, &GeneratorPanel::burstTriggered, &m_session, &Session::triggerBurst);
    connect(&m_session, &Session::generatorChanged, this, [this](const GenConfig &g) {
        ui->generatorPanel->showConfig(g);
        showGeneratorStatus();
    });
    connect(ui->analyzerButton, &QToolButton::clicked, this, [this] {
        m_session.setActivity(DeviceLink::Analyzer);
        showActivity(m_session.activity());  // until the unit has switched
    });
    connect(ui->generatorButton, &QToolButton::clicked, this, [this] {
        m_session.setActivity(DeviceLink::Generator);
        showActivity(m_session.activity());
    });
    connect(&m_session, &Session::activityChanged, this, &MainWindow::showActivity);
    m_genLabel = new QLabel(this);
    m_genLabel->setObjectName("generatorStatusLabel");
    statusBar()->insertPermanentWidget(0, m_genLabel);  // where the analyzer's labels are
    ui->analyzerButton->setEnabled(false);  // until connected: the unit's mode is followed
    ui->generatorButton->setEnabled(false);
    // firmware before v1.4-266 can lose a command and stop answering: say so, offer the update
    ui->firmwareWarningLabel->hide();
    connect(ui->firmwareWarningLabel, &QLabel::linkActivated, ui->actionFirmware, &QAction::trigger);
}

void MainWindow::showActivity(int activity)
{
    const bool gen = activity == DeviceLink::Generator;
    ui->modeStack->setCurrentWidget(gen ? static_cast<QWidget *>(ui->generatorPanel) : ui->analyzerPage);
    ui->analyzerButton->setChecked(!gen);
    ui->generatorButton->setChecked(gen);
    if (gen && ui->actionRecord->isChecked())
        ui->actionRecord->setChecked(false);  // stops recording
    for (QAction *a : {ui->actionSaveCsv, ui->actionSaveImage, ui->actionCopyImage, ui->actionRecord})
        a->setEnabled(!gen);  // they act on the spectrum
    for (QShortcut *s : m_analyzerShortcuts)
        s->setEnabled(!gen);
    for (QLabel *l : {m_stepLabel, m_sweepsLabel, m_rateLabel})
        l->setVisible(!gen);
    showAverage();  // hidden on the generator page
    m_genLabel->setVisible(gen);
    showGeneratorStatus();
}

void MainWindow::showGeneratorStatus()
{
    m_genLabel->setText(tr("Generator · ") + ui->generatorPanel->statusText());
}

void MainWindow::escape()
{
    if (m_session.activity() == DeviceLink::Generator) {  // the generator's "stop": RF off
        GenConfig g = m_session.generator();
        g.on = false;
        m_session.setGenerator(g);
    } else {
        m_session.setScan(DeviceLink::Off);
    }
}
