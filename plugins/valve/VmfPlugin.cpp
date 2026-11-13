// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch plugin: Valve Hammer map (.vmf) export (ValveFormat.h).
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

class VmfPlugin : public QObject, public gs::FormatPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID GNUSKETCH_FORMAT_PLUGIN_IID FILE "vmf.json")
    Q_INTERFACES(gs::FormatPlugin)

public:
    QString formatName() const override { return QStringLiteral("Valve Hammer Map"); }
    QStringList extensions() const override { return {QStringLiteral("vmf")}; }
    QString description() const override {
        return QStringLiteral("Source engine levels for Hammer: convex pieces as brushes, components named after entity classes "
                              "(info_player_start, light, npc_...) as entities, lights and the sun as light entities.");
    }
    bool canExport() const override { return true; }
    bool exportFile(const gs::Document& doc, const QString& path, QStringList* messages) override {
        gs::valve::VmfOptions o;
        QSettings s;
        o.skyname = s.value("valve/skyname", QString::fromStdString(o.skyname)).toString().toStdString();
        o.defaultMaterial = s.value("valve/defaultMaterial", QString::fromStdString(o.defaultMaterial)).toString().toStdString();
        o.sealBox = s.value("valve/sealBox", o.sealBox).toBool();
        // With a window to ask in: the options, remembered.
        if (qobject_cast<QApplication*>(QCoreApplication::instance()) && !qEnvironmentVariableIsSet("GNUSKETCH_NO_DIALOGS")) {
            QDialog dlg(QApplication::activeWindow());
            dlg.setWindowTitle(QStringLiteral("Export Valve Hammer Map"));
            auto* form = new QFormLayout(&dlg);
            auto* note = new QLabel(QStringLiteral("Every closed, convex piece becomes a brush; components named like an entity class "
                                                   "(info_player_start, light, npc_zombie...) become entities. Units: inches."),
                                    &dlg);
            note->setWordWrap(true);
            form->addRow(note);
            auto* sky = new QLineEdit(QString::fromStdString(o.skyname), &dlg);
            sky->setToolTip(QStringLiteral("sky_day01_01 (Half-Life 2), sky_tf2_04 (Team Fortress 2), urbannightstormhdr (Left 4 Dead)..."));
            form->addRow(QStringLiteral("Skybox:"), sky);
            auto* mat = new QLineEdit(QString::fromStdString(o.defaultMaterial), &dlg);
            form->addRow(QStringLiteral("Material for unpainted faces:"), mat);
            auto* seal = new QCheckBox(QStringLiteral("Seal it in a skybox box (a test map that compiles without leaks)"), &dlg);
            seal->setChecked(o.sealBox);
            form->addRow(seal);
            auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            form->addRow(box);
            QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            if (dlg.exec() != QDialog::Accepted) {
                if (messages) messages->append(QStringLiteral("Cancelled."));
                return false;
            }
            o.skyname = sky->text().trimmed().toStdString();
            o.defaultMaterial = mat->text().trimmed().toStdString();
            o.sealBox = seal->isChecked();
            s.setValue("valve/skyname", sky->text().trimmed());
            s.setValue("valve/defaultMaterial", mat->text().trimmed());
            s.setValue("valve/sealBox", o.sealBox);
        }
        return gs::valve::writeVmf(doc, path, o, messages);
    }
};

#include "VmfPlugin.moc"
