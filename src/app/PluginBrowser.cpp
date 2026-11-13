// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginBrowser.h"

#include "Plugins.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gs {

PluginBrowser::PluginBrowser(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Plugins"));
    resize(820, 520);
    auto* lay = new QVBoxLayout(this);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search plugins, vendors, formats…"));
    search_->setClearButtonEnabled(true);
    lay->addWidget(search_);
    auto* split = new QSplitter(this);
    list_ = new QListWidget(split);
    list_->setObjectName(QStringLiteral("PluginList"));
    list_->setMinimumWidth(240);
    details_ = new QTextBrowser(split);
    details_->setObjectName(QStringLiteral("PluginDetails"));
    details_->setOpenExternalLinks(true);
    split->addWidget(list_);
    split->addWidget(details_);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);
    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setProperty("hint", true);
    lay->addWidget(note_);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    restart_ = box->addButton(QStringLiteral("Restart GNUSketch Now"), QDialogButtonBox::ActionRole);
    restart_->setEnabled(false);
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(restart_, &QPushButton::clicked, this, [this] {
        accept();
        emit restartRequested();
    });
    connect(search_, &QLineEdit::textChanged, this, [this] { fill(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { showDetails(); });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        const QString file = it->data(Qt::UserRole).toString();
        const bool on = it->checkState() == Qt::Checked;
        if (on == Plugins::isEnabled(file)) return;
        Plugins::setEnabled(file, on);
        changed_ = true;
        restart_->setEnabled(true);
        showDetails();
    });
    fill();
}

void PluginBrowser::fill() {
    const QString q = search_->text().trimmed();
    const QString current = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
    QSignalBlocker b(list_);
    list_->clear();
    for (const PluginInfo& p : Plugins::installed()) {
        const QString hay = p.name + ' ' + p.vendor + ' ' + p.license + ' ' + p.description + ' ' + p.extensions.join(' ');
        if (!q.isEmpty() && !hay.contains(q, Qt::CaseInsensitive)) continue;
        auto* it = new QListWidgetItem(p.version.isEmpty() ? p.name : QStringLiteral("%1  %2").arg(p.name, p.version), list_);
        it->setData(Qt::UserRole, p.file);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Plugins::isEnabled(p.file) ? Qt::Checked : Qt::Unchecked);
        it->setToolTip(QStringLiteral("%1 — %2").arg(p.vendor, p.license));
        if (p.file == current) list_->setCurrentItem(it);
    }
    if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
    showDetails();
    note_->setText(QStringLiteral("Tick a plugin to turn it on, untick to turn it off; this applies when GNUSketch starts again. "
                                  "Plugins are looked for in: %1")
                       .arg(Plugins::folders().join(QStringLiteral("; "))));
}

void PluginBrowser::showDetails() {
    const QListWidgetItem* it = list_->currentItem();
    if (!it) {
        details_->setHtml(Plugins::installed().empty() ? QStringLiteral("<p>No plugins found.</p>") : QString());
        return;
    }
    const QString file = it->data(Qt::UserRole).toString();
    for (const PluginInfo& p : Plugins::installed()) {
        if (p.file != file) continue;
        const bool on = Plugins::isEnabled(p.file);
        QString state = p.loaded ? QStringLiteral("<b style='color:#3c9a5f'>Loaded</b>")
                                 : p.enabled ? QStringLiteral("<b style='color:#c0392b'>Could not load</b>: %1").arg(p.problem.toHtmlEscaped())
                                             : QStringLiteral("<b>Turned off</b>");
        if (on != p.enabled) state += on ? QStringLiteral(" — will be turned on at the next start") : QStringLiteral(" — will be turned off at the next start");
        QStringList can;
        if (p.canImport) can << QStringLiteral("open and import");
        if (p.canExport) can << QStringLiteral("export");
        QString html = QStringLiteral("<h2 style='margin-bottom:2px'>%1</h2><p style='margin-top:0'>%2</p>")
                           .arg(p.name.toHtmlEscaped(), p.version.isEmpty() ? QString() : QStringLiteral("Version ") + p.version.toHtmlEscaped());
        html += QStringLiteral("<table cellpadding=3>");
        auto row = [&](const QString& k, const QString& v) {
            if (!v.isEmpty()) html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(k, v);
        };
        row(QStringLiteral("Vendor"), p.vendor.toHtmlEscaped());
        row(QStringLiteral("License"), p.license.toHtmlEscaped());
        row(QStringLiteral("Formats"), p.extensions.isEmpty() ? QString() : QStringLiteral(".") + p.extensions.join(QStringLiteral(", .")).toHtmlEscaped());
        row(QStringLiteral("Can"), can.join(QStringLiteral(", ")));
        row(QStringLiteral("State"), state);
        row(QStringLiteral("Website"), p.url.isEmpty() ? QString() : QStringLiteral("<a href='%1'>%1</a>").arg(p.url.toHtmlEscaped()));
        row(QStringLiteral("File"), QStringLiteral("<small>%1</small>").arg(p.file.toHtmlEscaped()));
        html += QStringLiteral("</table>");
        if (!p.description.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(p.description.toHtmlEscaped());
        if (!p.credits.isEmpty()) html += QStringLiteral("<p><small><b>Credits:</b> %1</small></p>").arg(p.credits.toHtmlEscaped());
        details_->setHtml(html);
    }
}

} // namespace gs
