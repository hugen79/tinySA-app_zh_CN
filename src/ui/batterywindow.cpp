#include "batterywindow.h"
#include "ui_batterywindow.h"
#include <QDateTime>
#include <QFileDialog>

BatteryWindow::BatteryWindow(QWidget *parent) : QWidget(parent, Qt::Window), ui(new Ui::BatteryWindow)
{
    ui->setupUi(this);
    QCustomPlot *p = ui->batteryPlot;
    p->addGraph()->setLineStyle(QCPGraph::lsLine);
    p->graph(0)->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, 3));
    auto time = QSharedPointer<QCPAxisTickerDateTime>::create();
    time->setDateTimeFormat("hh:mm");
    p->xAxis->setTicker(time);
    p->xAxis->setLabel(tr("Time"));
    p->yAxis->setLabel(tr("Battery (V)"));
    p->yAxis->setRange(3.0, 4.3);
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    p->xAxis->setRange(now, now + 600);
    connect(ui->clearBatteryButton, &QPushButton::clicked, this, &BatteryWindow::clear);
    connect(ui->saveBatteryButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, "Save image", "tinySA battery.png",
                                                          "PNG (*.png);;JPEG (*.jpg)");
        if (!path.isEmpty())
            ui->batteryPlot->grab().save(path);
    });
    setTheme(Theme{});
}

BatteryWindow::~BatteryWindow() = default;

void BatteryWindow::setTheme(const Theme &t)
{
    QCustomPlot *p = ui->batteryPlot;
    p->setBackground(t.background);
    for (QCPAxis *ax : {p->xAxis, p->yAxis}) {
        ax->setBasePen(QPen(t.text));
        ax->setTickPen(QPen(t.text));
        ax->setSubTickPen(Qt::NoPen);
        ax->setTickLabelColor(t.text);
        ax->setLabelColor(t.text);
        ax->grid()->setPen(QPen(t.grid, 1, Qt::DashLine));
    }
    p->graph(0)->setPen(QPen(t.trace[0], t.lineWidth));
    p->replot(QCustomPlot::rpQueuedReplot);
}

int BatteryWindow::samples() const
{
    return ui->batteryPlot->graph(0)->dataCount();
}

void BatteryWindow::addSample(int mV)
{
    QCustomPlot *p = ui->batteryPlot;
    QCPGraph *g = p->graph(0);
    g->addData(QDateTime::currentMSecsSinceEpoch() / 1000.0, mV / 1000.0);
    bool found = false;
    const QCPRange y = g->getValueRange(found);
    const QCPRange x = g->getKeyRange(found);
    p->yAxis->setRange(std::min(3.0, y.lower - 0.05), std::max(4.3, y.upper + 0.05));
    p->xAxis->setRange(x.lower, std::max(x.upper, x.lower + 600));  // at least 10 minutes
    ui->batteryReadout->setText(QString("%1 V   (min %2 V, max %3 V, %4 readings)")
                                    .arg(mV / 1000.0, 0, 'f', 3).arg(y.lower, 0, 'f', 3)
                                    .arg(y.upper, 0, 'f', 3).arg(g->dataCount()));
    p->replot(QCustomPlot::rpQueuedReplot);
}

void BatteryWindow::clear()
{
    ui->batteryPlot->graph(0)->data()->clear();
    ui->batteryReadout->setText(tr("Cleared"));
    ui->batteryPlot->replot();
}
