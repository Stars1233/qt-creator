// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "qmljstools_global.h"

#include <coreplugin/dialogs/ioptionspage.h>

#include <texteditor/icodestylepreferences.h>
#include <texteditor/codestyleeditor.h>

#include <utils/filepath.h>
#include <utils/store.h>

namespace TextEditor {
class FontSettings;
class TabSettings;
}

QT_BEGIN_NAMESPACE
class QStackedWidget;
QT_END_NAMESPACE

namespace QmlJSTools {

class FormatterSelectionWidget;

class QMLJSTOOLS_EXPORT QmlJSCodeStyleSettings
{
public:
    QmlJSCodeStyleSettings();

    enum Formatter {
        Builtin,
        QmlFormat,
        Custom
    };

    int lineLength = 80;
    QString qmlformatIniContent;
    Formatter formatter = Builtin;
    Utils::FilePath customFormatterPath;
    QString customFormatterArguments;

    Utils::Store toMap() const;
    void fromMap(const Utils::Store &map);

    bool equals(const QmlJSCodeStyleSettings &rhs) const;
    bool operator==(const QmlJSCodeStyleSettings &s) const { return equals(s); }
    bool operator!=(const QmlJSCodeStyleSettings &s) const { return !equals(s); }

    static QmlJSCodeStyleSettings currentGlobalCodeStyle();
    static TextEditor::TabSettings currentGlobalTabSettings();
    static Utils::Id settingsId();
};

using QmlJSCodeStylePreferences = TextEditor::TypedCodeStylePreferences<QmlJSCodeStyleSettings>;

namespace Internal {

class QmlJSCodeStylePreferencesWidget : public TextEditor::CodeStyleEditorWidget
{
    Q_OBJECT

public:
    explicit QmlJSCodeStylePreferencesWidget(const QString &previewText, QWidget *parent = nullptr);

    void setPreferences(QmlJSCodeStylePreferences* preferences);

private:
    void decorateEditor(const TextEditor::FontSettings &fontSettings);
    void setVisualizeWhitespace(bool on);
    void slotSettingsChanged(const QmlJSCodeStyleSettings &);
    void slotCurrentPreferencesChanged(TextEditor::ICodeStylePreferences *preferences);
    void updatePreview();
    void builtInFormatterPreview();
    void qmlformatPreview();
    void customFormatterPreview();

    FormatterSelectionWidget *m_formatterSelectionWidget;
    QStackedWidget *m_formatterSettingsStack;
    TextEditor::SnippetEditorWidget *m_previewTextEdit;
    QmlJSCodeStylePreferences *m_preferences = nullptr;
};

class QmlJSCodeStyleSettingsPage : public Core::IOptionsPage
{
public:
    QmlJSCodeStyleSettingsPage();
};

} // namespace Internal
} // namespace QmlJSTools

Q_DECLARE_METATYPE(QmlJSTools::QmlJSCodeStyleSettings)
