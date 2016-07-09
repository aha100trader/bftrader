#include "configdialog.h"
#include "profile.h"
#include "servicemgr.h"
#include "ui_configdialog.h"

ConfigDialog::ConfigDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::ConfigDialog)
{
    ui->setupUi(this);
    setWindowTitle("config");
    setWindowIcon(QIcon(":/images/gateway.png"));
}

ConfigDialog::~ConfigDialog()
{
    delete ui;
}

void ConfigDialog::save()
{
    Profile* profile = g_sm->profile();
    profile->put("userId", ui->userId->text());
    profile->put("brokerId", ui->brokerId->text());

    // 解决ctp网址缺tcp://或者大写导致的crash=
    QString frontMd = ui->frontMd->text();
    if (!frontMd.trimmed().toLower().startsWith("tcp://")) {
        frontMd = QString("tcp://") + frontMd.trimmed().toLower();
    } else {
        frontMd = frontMd.trimmed().toLower();
    }
    profile->put("frontMd", frontMd);

    // 解决ctp网址缺tcp://或者大写导致的crash=
    QString frontTd = ui->frontTd->text();
    if (!frontTd.trimmed().toLower().startsWith("tcp://")) {
        frontTd = QString("tcp://") + frontTd.trimmed().toLower();
    } else {
        frontTd = frontTd.trimmed().toLower();
    }
    profile->put("frontTd", frontTd);

    profile->put("symbolPrefixes", ui->symbolPrefixes->text());
    profile->put("filterTick", ui->checkBoxFilterTick->isChecked());
    profile->commit();
}

void ConfigDialog::load()
{
    Profile* profile = g_sm->profile();
    ui->userId->setText(profile->get("userId", "666666").toString());
    ui->brokerId->setText(profile->get("brokerId", "9999").toString());
    ui->frontMd->setText(profile->get("frontMd", "tcp://180.168.146.187:10031").toString());
    ui->frontTd->setText(profile->get("frontTd", "tcp://180.168.146.187:10030").toString());
    ui->symbolPrefixes->setText(profile->get("symbolPrefixes", "ic;sr;rb;pp;m1").toString());
    ui->checkBoxFilterTick->setChecked(profile->get("filterTick", false).toBool());
}
