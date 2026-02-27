// Copyright (C) 2026 Sean Moon
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "HtopWidget.h"

#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include <QFile>
#include <unistd.h>

// ── Data Structures ───────────────────────────────────────────────────────────

namespace {

struct CoreSnapshot {
    quint64 active = 0;
    quint64 total  = 0;
};

struct CpuData {
    CoreSnapshot         total;
    QVector<CoreSnapshot> cores;
};

struct MemInfo {
    quint64 totalKb     = 0;
    quint64 usedKb      = 0;
    quint64 swapTotalKb = 0;
    quint64 swapUsedKb  = 0;
};

struct LoadInfo {
    double  load1         = 0.0;
    double  load5         = 0.0;
    double  load15        = 0.0;
    int     running       = 0;
    int     total         = 0;
    quint64 uptimeSecs    = 0;
};

struct ProcEntry {
    int     pid   = 0;
    QString name;
    quint64 ticks = 0;   // utime + stime
    quint64 rssKb = 0;
};

struct ProcInfo {
    int     pid    = 0;
    QString name;
    double  cpuPct = 0.0;
    double  memPct = 0.0;
    quint64 rssKb  = 0;
};

// ── Platform Readers ──────────────────────────────────────────────────────────

static CoreSnapshot parseCpuLine(const QList<QByteArray>& parts) {
    if (parts.size() < 5) return {};
    quint64 user    = parts[1].toULongLong();
    quint64 nice    = parts[2].toULongLong();
    quint64 system  = parts[3].toULongLong();
    quint64 idle    = parts[4].toULongLong();
    quint64 iowait  = parts.size() > 5 ? parts[5].toULongLong() : 0;
    quint64 irq     = parts.size() > 6 ? parts[6].toULongLong() : 0;
    quint64 softirq = parts.size() > 7 ? parts[7].toULongLong() : 0;
    quint64 active  = user + nice + system + irq + softirq;
    quint64 total   = active + idle + iowait;
    return {active, total};
}

CpuData readCpuData() {
    QFile f("/proc/stat");
    if (!f.open(QFile::ReadOnly)) return {};
    // /proc files report size=0, so atEnd() returns true immediately.
    // Read everything at once and split by line.
    CpuData data;
    for (const QByteArray& raw : f.readAll().split('\n')) {
        const QByteArray line = raw.simplified();
        const auto parts = line.split(' ');
        if (parts.isEmpty() || !parts[0].startsWith("cpu")) break;
        if (parts[0] == "cpu")
            data.total = parseCpuLine(parts);
        else
            data.cores.append(parseCpuLine(parts));
    }
    return data;
}

MemInfo readMemInfo() {
    QFile f("/proc/meminfo");
    if (!f.open(QFile::ReadOnly)) return {};
    MemInfo m;
    quint64 memAvail = 0, swapFree = 0;
    for (const QByteArray& raw : f.readAll().split('\n')) {
        const auto parts = raw.simplified().split(' ');
        if (parts.size() < 2) continue;
        quint64 val = parts[1].toULongLong();
        const auto& key = parts[0];
        if      (key == "MemTotal:")     m.totalKb     = val;
        else if (key == "MemAvailable:") memAvail      = val;
        else if (key == "SwapTotal:")    m.swapTotalKb = val;
        else if (key == "SwapFree:")     swapFree      = val;
    }
    m.usedKb     = m.totalKb > memAvail ? m.totalKb - memAvail : 0;
    m.swapUsedKb = m.swapTotalKb > swapFree ? m.swapTotalKb - swapFree : 0;
    return m;
}

LoadInfo readLoadInfo() {
    LoadInfo li;
    {
        QFile f("/proc/loadavg");
        if (f.open(QFile::ReadOnly)) {
            const auto parts = f.readLine().simplified().split(' ');
            if (parts.size() >= 4) {
                li.load1  = parts[0].toDouble();
                li.load5  = parts[1].toDouble();
                li.load15 = parts[2].toDouble();
                const auto tp = parts[3].split('/');
                if (tp.size() == 2) { li.running = tp[0].toInt(); li.total = tp[1].toInt(); }
            }
        }
    }
    {
        QFile f("/proc/uptime");
        if (f.open(QFile::ReadOnly))
            li.uptimeSecs = static_cast<quint64>(f.readLine().simplified().split(' ').value(0).toDouble());
    }
    return li;
}

QVector<ProcEntry> readAllProcs(quint64 pageSize) {
    QVector<ProcEntry> procs;
    const QDir procDir("/proc");
    for (const QString& dname : procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        int pid = dname.toInt(&ok);
        if (!ok) continue;

        QFile f(QString("/proc/%1/stat").arg(pid));
        if (!f.open(QFile::ReadOnly)) continue;
        const QByteArray data = f.readAll();

        int nameStart = data.indexOf('(');
        int nameEnd   = data.lastIndexOf(')');
        if (nameStart < 0 || nameEnd <= nameStart) continue;

        ProcEntry e;
        e.pid  = pid;
        e.name = QString::fromLatin1(data.mid(nameStart + 1, nameEnd - nameStart - 1));

        // After ')': [0]=state [1]=ppid ... [11]=utime [12]=stime ... [21]=rss
        const auto fields = data.mid(nameEnd + 2).split(' ');
        if (fields.size() > 21) {
            e.ticks = fields[11].toULongLong() + fields[12].toULongLong();
            e.rssKb = fields[21].toULongLong() * pageSize / 1024;
        }
        procs.append(e);
    }
    return procs;
}

// ── Formatters ────────────────────────────────────────────────────────────────

QString formatKb(quint64 kb) {
    if (kb < 1024)
        return QString("%1K").arg(kb);
    if (kb < 1024 * 1024)
        return QString("%1M").arg(kb / 1024.0, 0, 'f', 1);
    return QString("%1G").arg(kb / (1024.0 * 1024.0), 0, 'f', 2);
}

QString formatUptime(quint64 s) {
    quint64 d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (d > 0)
        return QString("%1d %2:%3:%4").arg(d)
            .arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    return QString("%1:%2:%3")
        .arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

QColor cpuBarColor(int pct) {
    if (pct < 50) return QColor(0x00, 0xcc, 0x44);  // green
    if (pct < 80) return QColor(0xcc, 0xbb, 0x00);  // yellow
    return             QColor(0xcc, 0x33, 0x33);     // red
}

QString barStyle(const QColor& c) {
    return QString(
        "QProgressBar { background: #1a1a2e; border: none; border-radius: 2px; }"
        "QProgressBar::chunk { background: %1; border-radius: 2px; }").arg(c.name());
}

}  // namespace

// ── HtopDisplay ───────────────────────────────────────────────────────────────

class HtopDisplay : public QWidget {
    Q_OBJECT

public:
    explicit HtopDisplay(QWidget* parent = nullptr) : QWidget(parent) {
        pageSize_ = static_cast<quint64>(sysconf(_SC_PAGESIZE));
        // Prime delta snapshots before the first tick
        {
            CpuData cd = readCpuData();
            prevCores_      = cd.cores;
            prevTotalTicks_ = cd.total.total;
        }
        {
            MemInfo mi = readMemInfo();
            memTotalKb_ = mi.totalKb;
        }
        for (const auto& e : readAllProcs(pageSize_))
            prevProcTicks_[e.pid] = e.ticks;

        setupUi();

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &HtopDisplay::tick);
        timer->start(1000);
    }

private:
    // ── UI setup ──────────────────────────────────────────────────────────────
    void setupUi() {
        setStyleSheet(
            "QWidget { background: transparent; }"
            "QTreeWidget {"
            "  background: #0d1117; color: #c8cee8;"
            "  border: none; font-family: monospace; font-size: 11px; outline: 0; }"
            "QTreeWidget::item { padding: 1px 4px; }"
            "QTreeWidget::item:alternate { background: #111827; }"
            "QTreeWidget::item:selected { background: #1f3060; color: #e0e8ff; }"
            "QHeaderView::section {"
            "  background: #161b22; color: #5588cc;"
            "  border: none; border-bottom: 1px solid #2d3748;"
            "  padding: 2px 6px; font-size: 10px; font-weight: 700; letter-spacing: 1px; }"
            "QScrollBar:vertical { background: #0d1117; width: 6px; border: none; }"
            "QScrollBar::handle:vertical { background: #2d3748; border-radius: 3px; min-height: 20px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

        auto* vbox = new QVBoxLayout(this);
        vbox->setContentsMargins(8, 8, 8, 6);
        vbox->setSpacing(4);

        // ── Summary: 3 rows × 2 columns ───────────────────────────────────────
        // Col 0: Mem bar (row 0), Swp bar (row 1)
        // Col 1: Tasks (row 0), Load (row 1), Up (row 2)
        auto* summaryGrid = new QGridLayout();
        summaryGrid->setHorizontalSpacing(12);
        summaryGrid->setVerticalSpacing(3);
        summaryGrid->setColumnStretch(0, 3);
        summaryGrid->setColumnStretch(1, 2);

        auto makeMemWidget = [&](const QString& label, QProgressBar*& bar, QLabel*& valLbl) -> QWidget* {
            auto* w  = new QWidget(this);
            auto* hl = new QHBoxLayout(w);
            hl->setContentsMargins(0, 0, 0, 0);
            hl->setSpacing(5);
            auto* lbl = new QLabel(label, w);
            lbl->setStyleSheet("color: #5588cc; font-size: 11px; font-weight: bold;");
            lbl->setFixedWidth(28);
            bar = new QProgressBar(w);
            bar->setRange(0, 1000);
            bar->setValue(0);
            bar->setTextVisible(false);
            bar->setFixedHeight(12);
            bar->setStyleSheet(barStyle(QColor(0x00, 0x99, 0x44)));
            valLbl = new QLabel("--/--", w);
            valLbl->setStyleSheet("color: #9090b8; font-size: 10px; font-family: monospace;");
            valLbl->setFixedWidth(84);
            valLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            hl->addWidget(lbl);
            hl->addWidget(bar, 1);
            hl->addWidget(valLbl);
            return w;
        };
        summaryGrid->addWidget(makeMemWidget("Mem", memBar_, memLabel_), 0, 0);
        summaryGrid->addWidget(makeMemWidget("Swp", swpBar_, swpLabel_), 1, 0);

        auto makeInfoLabel = [&](QLabel*& lbl, const QString& init) -> QLabel* {
            lbl = new QLabel(init, this);
            lbl->setStyleSheet("color: #9090b8; font-size: 10px; font-family: monospace;");
            return lbl;
        };
        summaryGrid->addWidget(makeInfoLabel(taskLabel_,   "Tasks: --"),        0, 1);
        summaryGrid->addWidget(makeInfoLabel(loadLabel_,   "Load:  -- -- --"),  1, 1);
        summaryGrid->addWidget(makeInfoLabel(uptimeLabel_, "Up:    --"),         2, 1);

        vbox->addLayout(summaryGrid);

        // Separator
        auto* sep1 = new QFrame(this);
        sep1->setFrameShape(QFrame::HLine);
        sep1->setStyleSheet("color: #2d3748;");
        vbox->addWidget(sep1);

        // ── CPU cores: 2-column grid ───────────────────────────────────────────
        // Core i is at grid row i/2, column i%2 (0-indexed).
        cpuPanel_ = new QWidget(this);
        cpuPanelLayout_ = new QGridLayout(cpuPanel_);
        cpuPanelLayout_->setContentsMargins(0, 0, 0, 0);
        cpuPanelLayout_->setSpacing(3);
        buildCpuRows(prevCores_.size());
        vbox->addWidget(cpuPanel_);

        // Separator
        auto* sep2 = new QFrame(this);
        sep2->setFrameShape(QFrame::HLine);
        sep2->setStyleSheet("color: #2d3748;");
        vbox->addWidget(sep2);

        // ── Process table ──────────────────────────────────────────────────────
        procTable_ = new QTreeWidget(this);
        procTable_->setColumnCount(4);
        procTable_->setHeaderLabels({"PID", "NAME", "CPU%", "MEM%"});
        procTable_->setSortingEnabled(false);
        procTable_->setRootIsDecorated(false);
        procTable_->setUniformRowHeights(true);
        procTable_->setAlternatingRowColors(true);
        procTable_->setFocusPolicy(Qt::NoFocus);
        procTable_->setSelectionMode(QAbstractItemView::NoSelection);
        // Ensure at least 6 process rows are always visible (~17px/row + 22px header).
        procTable_->setMinimumHeight(124);

        auto* hdr = procTable_->header();
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(0, QHeaderView::Fixed);
        hdr->setSectionResizeMode(1, QHeaderView::Stretch);
        hdr->setSectionResizeMode(2, QHeaderView::Fixed);
        hdr->setSectionResizeMode(3, QHeaderView::Fixed);
        procTable_->setColumnWidth(0, 52);
        procTable_->setColumnWidth(2, 48);
        procTable_->setColumnWidth(3, 48);

        vbox->addWidget(procTable_, 1);
    }

    void buildCpuRows(int count) {
        coreBars_.clear();
        corePctLabels_.clear();

        // Remove any existing widgets from the grid
        while (QLayoutItem* item = cpuPanelLayout_->takeAt(0)) {
            if (QWidget* w = item->widget()) {
                w->hide();
                w->deleteLater();
            }
            delete item;
        }

        // Both columns share equal width
        cpuPanelLayout_->setColumnStretch(0, 1);
        cpuPanelLayout_->setColumnStretch(1, 1);

        const int labelW = count >= 10 ? 20 : 14;

        for (int i = 0; i < count; ++i) {
            const int gridRow = i / 2;
            const int gridCol = i % 2;

            auto* container = new QWidget(cpuPanel_);
            container->setStyleSheet("background: transparent;");
            auto* hl = new QHBoxLayout(container);
            hl->setSpacing(4);
            // Small left margin on the right column to separate the two halves
            hl->setContentsMargins(gridCol == 1 ? 6 : 0, 0, 0, 0);

            auto* numLbl = new QLabel(QString::number(i + 1), container);
            numLbl->setFixedWidth(labelW);
            numLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            numLbl->setStyleSheet("color: #5588cc; font-size: 10px; font-weight: bold; background: transparent;");

            auto* bar = new QProgressBar(container);
            bar->setRange(0, 100);
            bar->setValue(0);
            bar->setTextVisible(false);
            bar->setFixedHeight(12);
            bar->setStyleSheet(barStyle(QColor(0x00, 0xcc, 0x44)));

            auto* pctLbl = new QLabel("  0%", container);
            pctLbl->setFixedWidth(32);
            pctLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            pctLbl->setStyleSheet("color: #c8cee8; font-size: 10px; font-family: monospace; background: transparent;");

            hl->addWidget(numLbl);
            hl->addWidget(bar, 1);
            hl->addWidget(pctLbl);

            cpuPanelLayout_->addWidget(container, gridRow, gridCol);
            coreBars_.append(bar);
            corePctLabels_.append(pctLbl);
        }

        if (count == 0) {
            auto* placeholder = new QLabel("No CPU data", cpuPanel_);
            placeholder->setStyleSheet("color: #606080; font-size: 10px;");
            cpuPanelLayout_->addWidget(placeholder, 0, 0, 1, 2);
        }
    }

    // ── Tick ──────────────────────────────────────────────────────────────────
    void tick() {
        CpuData cd = readCpuData();
        tickCpu(cd);
        tickMem();
        tickLoad();
        tickProcs(cd.total.total);
    }

    void tickCpu(const CpuData& cd) {
        if (cd.cores.size() != prevCores_.size()) {
            buildCpuRows(cd.cores.size());
            prevCores_ = cd.cores;
            return;
        }
        for (int i = 0; i < cd.cores.size() && i < coreBars_.size(); ++i) {
            quint64 dTotal  = cd.cores[i].total  - prevCores_[i].total;
            quint64 dActive = cd.cores[i].active - prevCores_[i].active;
            int pct = (dTotal > 0) ? qBound(0, int(dActive * 100 / dTotal), 100) : 0;

            QColor color = cpuBarColor(pct);
            coreBars_[i]->setStyleSheet(barStyle(color));
            coreBars_[i]->setValue(pct);

            corePctLabels_[i]->setText(QString("%1%").arg(pct, 3));
            corePctLabels_[i]->setStyleSheet(
                QString("color: %1; font-size: 10px; font-family: monospace; background: transparent;")
                    .arg(color.name()));
        }
        prevCores_ = cd.cores;
    }

    void tickMem() {
        MemInfo mi = readMemInfo();
        memTotalKb_ = mi.totalKb;

        if (mi.totalKb > 0) {
            memBar_->setValue(int(mi.usedKb * 1000 / mi.totalKb));
            memLabel_->setText(QString("%1/%2")
                .arg(formatKb(mi.usedKb), formatKb(mi.totalKb)));
        }

        if (mi.swapTotalKb > 0) {
            swpBar_->setValue(int(mi.swapUsedKb * 1000 / mi.swapTotalKb));
            swpLabel_->setText(QString("%1/%2")
                .arg(formatKb(mi.swapUsedKb), formatKb(mi.swapTotalKb)));
        } else {
            swpBar_->setValue(0);
            swpLabel_->setText("none");
        }
    }

    void tickLoad() {
        LoadInfo li = readLoadInfo();
        taskLabel_->setText(QString("Tasks: %1, %2 run").arg(li.total).arg(li.running));
        loadLabel_->setText(QString("Load:  %1 %2 %3")
            .arg(li.load1,  0, 'f', 2).arg(li.load5,  0, 'f', 2).arg(li.load15, 0, 'f', 2));
        uptimeLabel_->setText(QString("Up:    %1").arg(formatUptime(li.uptimeSecs)));
    }

    void tickProcs(quint64 currTotalTicks) {
        auto currProcs = readAllProcs(pageSize_);

        quint64 totalDelta = currTotalTicks > prevTotalTicks_
                           ? currTotalTicks - prevTotalTicks_
                           : 1;
        prevTotalTicks_ = currTotalTicks;

        QVector<ProcInfo> infos;
        infos.reserve(currProcs.size());

        QMap<int, quint64> newProcTicks;
        for (const auto& e : currProcs) {
            newProcTicks[e.pid] = e.ticks;

            ProcInfo pi;
            pi.pid   = e.pid;
            pi.name  = e.name;
            pi.rssKb = e.rssKb;
            quint64 delta = e.ticks > prevProcTicks_.value(e.pid, 0)
                          ? e.ticks - prevProcTicks_.value(e.pid, 0) : 0;
            // totalDelta is the sum across all cores; multiply by numCores to
            // get htop-style per-core percentage (100% = one full core).
            const int numCores = qMax(1, int(prevCores_.size()));
            pi.cpuPct = totalDelta > 0 ? (delta * 100.0 * numCores / totalDelta) : 0.0;
            pi.memPct = memTotalKb_ > 0 ? (e.rssKb * 100.0 / memTotalKb_) : 0.0;
            infos.append(pi);
        }
        prevProcTicks_ = std::move(newProcTicks);

        // Sort: primary CPU%, secondary MEM%
        std::sort(infos.begin(), infos.end(), [](const ProcInfo& a, const ProcInfo& b) {
            if (a.cpuPct != b.cpuPct) return a.cpuPct > b.cpuPct;
            return a.memPct > b.memPct;
        });

        constexpr int kMaxRows = 50;
        const int showCount = qMin(int(infos.size()), kMaxRows);

        procTable_->setUpdatesEnabled(false);

        // Grow or shrink item count to match, avoiding full clear
        while (procTable_->topLevelItemCount() > showCount)
            delete procTable_->takeTopLevelItem(procTable_->topLevelItemCount() - 1);
        while (procTable_->topLevelItemCount() < showCount) {
            auto* item = new QTreeWidgetItem(procTable_);
            item->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        }

        for (int i = 0; i < showCount; ++i) {
            const auto& pi = infos[i];
            auto* item = procTable_->topLevelItem(i);
            item->setText(0, QString::number(pi.pid));
            item->setText(1, pi.name);
            item->setText(2, pi.cpuPct < 0.05 ? "0.0" : QString::number(pi.cpuPct, 'f', 1));
            item->setText(3, QString::number(pi.memPct, 'f', 1));

            // Color the CPU% column based on load
            QColor col = pi.cpuPct < 0.5  ? QColor("#606080")
                       : pi.cpuPct < 10.0 ? QColor("#9090c0")
                       : pi.cpuPct < 50.0 ? QColor("#00cc44")
                       : pi.cpuPct < 80.0 ? QColor("#ccbb00")
                       :                    QColor("#cc3333");
            item->setForeground(2, col);
        }

        procTable_->setUpdatesEnabled(true);
    }

    // ── Member variables ──────────────────────────────────────────────────────

    // CPU panel
    QWidget*     cpuPanel_       = nullptr;
    QGridLayout* cpuPanelLayout_ = nullptr;
    QVector<QProgressBar*> coreBars_;
    QVector<QLabel*>       corePctLabels_;

    // Mem / Swap
    QProgressBar* memBar_  = nullptr;
    QProgressBar* swpBar_  = nullptr;
    QLabel*       memLabel_ = nullptr;
    QLabel*       swpLabel_ = nullptr;

    // System info
    QLabel* taskLabel_   = nullptr;
    QLabel* loadLabel_   = nullptr;
    QLabel* uptimeLabel_ = nullptr;

    // Process table
    QTreeWidget* procTable_ = nullptr;

    // Delta state
    QVector<CoreSnapshot> prevCores_;
    quint64               prevTotalTicks_ = 0;
    QMap<int, quint64>    prevProcTicks_;
    quint64               memTotalKb_ = 0;
    quint64               pageSize_   = 4096;
};

#include "HtopWidget.moc"

// ── HtopWidget (IWidget plugin) ───────────────────────────────────────────────

HtopWidget::HtopWidget(QObject* parent) : QObject(parent) {}

void HtopWidget::initialize(dashboard::WidgetContext* /*context*/) {}

QWidget* HtopWidget::createWidget(QWidget* parent) {
    return new HtopDisplay(parent);
}

QJsonObject HtopWidget::serialize() const { return {}; }
void HtopWidget::deserialize(const QJsonObject& /*data*/) {}

dashboard::WidgetMetadata HtopWidget::metadata() const {
    return {
        .name        = "htop",
        .version     = "1.0.0",
        .author      = "Dashboard",
        .description = "Per-core CPU, memory, and process monitor",
        .minSize     = QSize(360, 280),
        .maxSize     = QSize(900, 1200),
        .defaultSize = QSize(480, 520),
    };
}
