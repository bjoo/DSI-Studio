#ifndef DEVICETABLEWIDGET_H
#define DEVICETABLEWIDGET_H
#include <QTableWidget>
#include <QItemDelegate>
#include <QComboBox>
#include "tracking/device.h"
class tracking_window;
class DeviceTypeDelegate : public QItemDelegate
{
    Q_OBJECT

    public:
    DeviceTypeDelegate(QObject *parent)
         : QItemDelegate(parent){}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const;
    void setEditorData(QWidget *editor, const QModelIndex &index) const;
          void setModelData(QWidget *editor, QAbstractItemModel *model,
                            const QModelIndex &index) const;
private slots:
    void emitCommitData();
 };


class DeviceTableWidget : public QTableWidget
{
    Q_OBJECT
protected:
    void contextMenuEvent ( QContextMenuEvent * event );
public:
    explicit DeviceTableWidget(tracking_window& cur_tracking_window,QWidget *parent = nullptr);
    // Header:
public:
    tracking_window& cur_tracking_window;
    std::vector<std::shared_ptr<Device> > devices;
signals:
    void need_update(void);
private:
    QString new_device_str;
public slots:
    void updateDevices(QTableWidgetItem* item);
    void check_status(int,int);
    void newDevice();
    void delete_all_devices();
    void delete_device();
    void load_device(void);
    void save_device(void);
    void save_all_devices(void);

};

#endif // DEVICETABLEWIDGET_H