/*
 * ty, a collection of GUI and command-line tools to manage Teensy devices
 *
 * Distributed under the MIT license (see LICENSE.txt or http://opensource.org/licenses/MIT)
 * Copyright (c) 2015 Niels Martignène <niels.martignene@gmail.com>
 */

#ifndef DESCRIPTOR_NOTIFIER_HH
#define DESCRIPTOR_NOTIFIER_HH

#ifdef _WIN32
    #include <QWinEventNotifier>
#else
    #include <QSocketNotifier>
#endif

#include <functional>
#include <vector>

#include "ty.h"

class DescriptorNotifier : public QObject {
    Q_OBJECT

#ifdef _WIN32
    std::vector<QWinEventNotifier *> notifiers_;
#else
    std::vector<QSocketNotifier *> notifiers_;
#endif

    bool enabled_ = true;

public:
    DescriptorNotifier(QObject *parent = nullptr)
        : QObject(parent) {}
    DescriptorNotifier(ty_descriptor desc, QObject *parent = nullptr);
    DescriptorNotifier(ty_descriptor_set *set, QObject *parent = nullptr);

    void addDescriptorSet(ty_descriptor_set *set);
    void addDescriptor(ty_descriptor desc);

    void setDescriptorSet(ty_descriptor_set *set)
    {
        clear();
        addDescriptorSet(set);
    }
    void setDescriptor(ty_descriptor desc)
    {
        clear();
        addDescriptor(desc);
    }

    bool isEnabled() const { return enabled_; }

public slots:
    void setEnabled(bool enable);
    void clear();

signals:
    void activated(ty_descriptor desc);

private:
    void execute(std::function<void()> f);

private slots:
    void executeAsync(std::function<void()> f);
};

#endif
