#pragma once
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QWidget>

struct PolishProfile {
    QTemporaryDir dir;
    QMap<QByteArray, QByteArray> previous;
    QSet<QByteArray> previouslySet;
    PolishProfile() {
        if (!dir.isValid()) qFatal("Cannot create isolated UI profile");
        const QMap<QByteArray, QString> paths{
            {"HOME", "home"}, {"XDG_CONFIG_HOME", "xdg-config"},
            {"XDG_DATA_HOME", "xdg-data"}, {"XDG_STATE_HOME", "xdg-state"},
            {"XDG_CACHE_HOME", "xdg-cache"}, {"HN_CONFIG_DIR", "config"},
            {"HN_DATA_DIR", "data"}, {"HN_STATE_DIR", "state"},
            {"HN_CACHE_DIR", "cache"}, {"HN_NOTES_DIR", "notes"}};
        for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
            if (qEnvironmentVariableIsSet(it.key().constData())) previouslySet.insert(it.key());
            previous.insert(it.key(), qgetenv(it.key().constData()));
            const QString path = dir.filePath(it.value());
            if (!QDir().mkpath(path)) qFatal("Cannot create isolated UI directory");
            qputenv(it.key().constData(), path.toUtf8());
        }
    }
    ~PolishProfile() {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (previouslySet.contains(it.key())) qputenv(it.key().constData(), it.value());
            else qunsetenv(it.key().constData());
        }
    }
};

inline QRect inWidget(const QWidget *child, const QWidget *ancestor) {
    return QRect(child->mapTo(ancestor, QPoint(0, 0)), child->size());
}
