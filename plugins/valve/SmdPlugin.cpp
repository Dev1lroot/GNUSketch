// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch plugin: Valve studio model (.smd + .qc) export (ValveFormat.h).
#include "ValveFormat.h"
#include "plugin/FormatPlugin.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QSettings>

class SmdPlugin : public QObject, public gs::FormatPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID GNUSKETCH_FORMAT_PLUGIN_IID FILE "smd.json")
    Q_INTERFACES(gs::FormatPlugin)

public:
    QString formatName() const override { return QStringLiteral("Valve Studio Model"); }
    QStringList extensions() const override { return {QStringLiteral("smd")}; }
    QString description() const override {
        return QStringLiteral("Source engine props: the model as a reference SMD (inches, Z up) and a .qc to compile it with studiomdl.");
    }
    bool canExport() const override { return true; }
    bool exportFile(const gs::Document& doc, const QString& path, QStringList* messages) override {
        gs::valve::SmdOptions o;
        QSettings s;
        o.writeQc = s.value("valve/writeQc", o.writeQc).toBool();
        o.staticProp = s.value("valve/staticProp", o.staticProp).toBool();
        o.surfaceProp = s.value("valve/surfaceProp", QString::fromStdString(o.surfaceProp)).toString().toStdString();
        if (qobject_cast<QApplication*>(QCoreApplication::instance()) && !qEnvironmentVariableIsSet("GNUSKETCH_NO_DIALOGS")) {
            QDialog dlg(QApplication::activeWindow());
            dlg.setWindowTitle(QStringLiteral("Export Valve Studio Model"));
            auto* form = new QFormLayout(&dlg);
            auto* qc = new QCheckBox(QStringLiteral("Write a .qc beside it (for studiomdl)"), &dlg);
            qc->setChecked(o.writeQc);
            form->addRow(qc);
            auto* stat = new QCheckBox(QStringLiteral("Static prop ($staticprop)"), &dlg);
            stat->setChecked(o.staticProp);
            form->addRow(stat);
            auto* surf = new QLineEdit(QString::fromStdString(o.surfaceProp), &dlg);
            surf->setToolTip(QStringLiteral("How it sounds and breaks: default, metal, wood, concrete, glass..."));
            form->addRow(QStringLiteral("Surface:"), surf);
            auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            form->addRow(box);
            QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            if (dlg.exec() != QDialog::Accepted) {
                if (messages) messages->append(QStringLiteral("Cancelled."));
                return false;
            }
            o.writeQc = qc->isChecked();
            o.staticProp = stat->isChecked();
            o.surfaceProp = surf->text().trimmed().toStdString();
            s.setValue("valve/writeQc", o.writeQc);
            s.setValue("valve/staticProp", o.staticProp);
            s.setValue("valve/surfaceProp", surf->text().trimmed());
        }
        return gs::valve::writeSmd(doc, path, o, messages);
    }
};

#include "SmdPlugin.moc"
