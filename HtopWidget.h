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

#pragma once

#include <dashboard/IWidget.h>

#include <QObject>

class HtopWidget : public QObject, public dashboard::IWidget {
    Q_OBJECT
    Q_INTERFACES(dashboard::IWidget)
    Q_PLUGIN_METADATA(IID IWidget_iid FILE "htop.json")

public:
    explicit HtopWidget(QObject* parent = nullptr);

    void initialize(dashboard::WidgetContext* context) override;
    QWidget* createWidget(QWidget* parent) override;
    QJsonObject serialize() const override;
    void deserialize(const QJsonObject& data) override;
    dashboard::WidgetMetadata metadata() const override;
};
