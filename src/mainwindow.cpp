#include <QShortcut>
#include <QCloseEvent>
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "codeeditor.h"
#include "version.h"
#include "thememanager.h"
#include "themeicons.h"
#include "fileicons.h"
#include "foldmanager.h"
#include "snippetmanager.h"
#include "rust_adapter.h"


#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QFileSystemModel>
#include <QDir>
#include <QDirIterator>
#include <QTextCursor>
#include <QApplication>
#include <QScreen>
#include <QRect>
#include <QProcess>
#include <QInputDialog>
#include <QLineEdit>
#include <QFontDialog>
#include "internals/settings_store.h"
#include <QFileInfo>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QDebug>
#include <QHBoxLayout>
#include <QSpacerItem>
#include <QDialog>
#include <QListWidget>
#include <QVBoxLayout>
#include <QToolTip>
#include <QUrl>
#include <QPair>
#include <QMouseEvent>
#include <QButtonGroup>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QFontComboBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QDesktopServices>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QAbstractAnimation>
#include <QFrame>
#include <QMenu>

#ifdef Q_OS_LINUX
#include <QApplication>
#include <QWidget>
#include <QTime>
#include <QTimer>
#include <QPainter>
#include <QDebug>

// Linux blur support - uses KWin's KWindowEffects if available
// This provides a blur behind the window similar to Windows Acrylic
static bool enableLinuxBlur(WId windowId, bool darkMode)
{
    // Try KWindowEffects first (KDE/KWin)
    // This requires linking against KF5WindowEffects or KWindowSystem
    // For now, we set a property that can be used by window rules
    if (QApplication *app = qobject_cast<QApplication*>(QApplication::instance())) {
        app->setProperty("kwin_blur", true);
        app->setProperty("kwin_blur_region", QRegion());
        
        // Also try GTK3/GTK4 client-side decorations if available
        app->setProperty("gtk_application_prefer_dark_theme", darkMode);
    }
    
    return true;
}
#endif

MainWindow::MainWindow(const QString &initialProject, const QStringList &initialFiles, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , selectedTheme(ThemeColorFamily::Default, ThemeMode::Light)
    , autoSaveTimer(new QTimer(this))
    , updater(RustBackend::instance()->updater())
    , configValidator(RustBackend::instance()->configValidator())
    , lspClient(RustBackend::instance()->lspClient())
    , m_previousEditorStackIndex(0)
    , m_workspace(RustBackend::instance()->workspace())
    , m_sessionManager(nullptr)
    , m_refactoringManager(new RefactoringManager(this))
    , m_codeLensManager(nullptr)
    , m_statusBarWidget(nullptr)
    , m_encodingManager(nullptr)
    , m_notificationCenter(nullptr)
{
    ui->setupUi(this);

    // Initialize modular UI components
    m_themeManager = new ThemeManager(this);
    // 主題切換時自動重新著色所有追蹤中的圖標，確保在各主題下都保持可見
    connect(m_themeManager, &ThemeManager::themeChanged,
            ThemeIcons::instance(), &ThemeIcons::recolorAll);
    // 檔案樹使用主題感知的圖標提供者，切換主題後需重新取得圖標。
    // 只喚醒裝飾角色：資料列數與順序都沒變，layoutChanged 會白白強迫整棵樹
    // 重新配置。
    connect(m_themeManager, &ThemeManager::themeChanged, this, [this]() {
        if (fileModel) {
            fileModel->refreshDecorationRole();
        }
        // 分頁列的圖標是已著色的 QIcon，不像 ThemeIcons 追蹤的元件會自動換色，
        // 因此主題切換後必須重新指定。
        if (tabBar)
            updateAllTabIcons();
    });

    // Give editor container rounded corners
    ui->editorContainer->setStyleSheet("QWidget#editorContainer { background-color: palette(window); border-radius: 14px; }");
    
    // Add margins to main layout so container rounded corners are visible
    if (QHBoxLayout *hLayout = qobject_cast<QHBoxLayout*>(ui->centralwidget->layout())) {
        hLayout->setContentsMargins(6, 6, 6, 6);
        hLayout->setSpacing(6);
    }
    ui->centralwidget->setStyleSheet("QWidget#centralwidget { background-color: palette(mid); }");

    ui->sidebarDrawer->setStyleSheet(R"(
        QWidget#sidebarDrawer {
            background-color: palette(window);
            border-radius: 14px;
        }
    )");

    // ── UI action routing ───────────────────────────────────────────────
    // Every title-bar interaction is routed through the Rust UiActionHandler.
    // Rust validates the action and decides the command; the connections
    // below only execute the decided commands (Qt's "drawer" role).
    UiActionBridge *uiActions = RustBackend::instance()->uiActions();

    // Execute the commands Rust decided (window controls come from the
    // platform frame, so only the minimize / maximize / close Rust→Qt route
    // remains — the title bar no longer emits those requests):
    connect(uiActions, &UiActionBridge::windowMinimizeRequested, this, &MainWindow::showMinimized);
    connect(uiActions, &UiActionBridge::windowToggleMaximizedRequested, this, [this]() {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(uiActions, &UiActionBridge::windowCloseRequested, this, &QWidget::close);

    // Enable mouse tracking for resize edge detection
    setMouseTracking(true);
    if (centralWidget())
        centralWidget()->setMouseTracking(true);

    // Execute the commands Rust decided:
    connect(uiActions, &UiActionBridge::sidebarToggleRequested, this, [this]() {
        // The settled state lives in m_sidebarCollapsed; the drawer's width is a
        // mid-animation value while a collapse is in flight, so reading it here
        // would make a fast double-toggle flip back to the same state.
        setSidebarCollapsed(!m_sidebarCollapsed);
    });
    connect(uiActions, &UiActionBridge::settingsOpenRequested, this, [this]() {
        on_action_editor_settings_triggered();
    });    connect(uiActions, &UiActionBridge::actionError, this, [](const QString &action, const QString &error) {
        qWarning() << "[ui-action] rejected:" << action << "—" << error;
    });

    // Apply initial theme through ThemeManager
    ThemeManager::Theme initialTheme(
        static_cast<ThemeManager::ColorFamily>(static_cast<int>(selectedTheme.family)),
        static_cast<ThemeManager::Mode>(static_cast<int>(selectedTheme.mode)),
        ThemeManager::Features(static_cast<ThemeManager::Feature>(static_cast<int>(selectedTheme.features)))
    );
    m_themeManager->applyTheme(initialTheme);

    loadRecentProjects();

    QString customThemeId = SettingsStore::instance().value("theme/custom", QString()).toString();
    if (!customThemeId.isEmpty()) {
        m_themeManager->applyCustomTheme(ThemeManager::pluginsDir(), customThemeId);
    } else {
        int legacyTheme = SettingsStore::instance().value("theme/selected", 0).toInt();
        selectedTheme = themeFromLegacyInt(legacyTheme);
        applyTheme(selectedTheme);
    }

    QScreen *screen = QApplication::primaryScreen();
    if (screen) {
        QRect available = screen->availableGeometry();
        resize(qMin(available.width() * 7 / 10, 900), 
               qMin(available.height() * 7 / 10, 800));
    }
    
    fileModel = new FileTreeModel(this);
    // 使用主題感知的圖標提供者，確保檔案樹圖標在淺/深色/高對比下都可見
    m_fileIconProvider = new ThemeFileIconProvider();
    fileModel->setIconProvider(m_fileIconProvider);
    fileModel->setRootPath(QDir::homePath());
    ui->fileTreeView->setModel(fileModel);
    ui->fileTreeView->setHeaderHidden(true);
    ui->fileTreeView->setColumnHidden(1, true);
    ui->fileTreeView->setColumnHidden(2, true);
    ui->fileTreeView->setColumnHidden(3, true);
    // Pin the row glyph size to the size ThemeFileIconProvider rasterises at.
    // QIcon would otherwise be asked for a size it never produced and scale the
    // result (same reasoning as the tabBar->setIconSize call below).
    ui->fileTreeView->setIconSize(
        QSize(ThemeIcons::FileTreeSize, ThemeIcons::FileTreeSize));
    // Row height: the global sheet pads items by 5px, so a 14px-font row with
    // 18px glyphs comes out at 20 + 10 = 30px. Padding the tree's own items to
    // 12px takes that to 44px — half as thick again — and the id selector keeps
    // this override ahead of the app-level QTreeView::item rule. The font-size
    // bumps the tree's text one notch over the 13px app default so it reads at
    // the same scale as the enlarged glyphs; nothing else inherits it.
    ui->fileTreeView->setStyleSheet(QStringLiteral(
        "QTreeView#fileTreeView { font-size: 14px; }"
        "QTreeView#fileTreeView::item { padding: 12px 6px; }"));
    // Enable right-click context menu on the file tree
    ui->fileTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->fileTreeView, &QTreeView::customContextMenuRequested,
            this, &MainWindow::on_fileTreeView_contextMenu);


    editorStack = ui->editorStack;
    tabBar = ui->tabBar;

    // The QTabWidget that hosts the editor pages keeps its own internal tab
    // bar, which duplicated the custom top tabBar (double tab rows, clicks
    // on one not reflected on the other). The top tabBar is the single
    // source of truth — hide the inner one.
    ui->tabWidget->tabBar()->hide();

    // Pin the tab glyph size instead of inheriting the style's tab icon size:
    // the icons are pre-tinted at 16 logical px, and QIcon would otherwise be
    // asked for a size it never rasterised and scale the result.
    tabBar->setIconSize(QSize(16, 16));

    // Wrap the top tab bar in a row with a "+" new-tab button at the end.
    {
        int barPos = ui->editorContainerLayout->indexOf(tabBar);
        ui->editorContainerLayout->removeWidget(tabBar);
        QWidget *tabBarRow = new QWidget(ui->editorContainer);
        tabBarRow->setObjectName("tabBarRow");
        QHBoxLayout *rowLayout = new QHBoxLayout(tabBarRow);
        rowLayout->setContentsMargins(0, 0, 4, 0);
        rowLayout->setSpacing(4);
        rowLayout->addWidget(tabBar, 1);
        m_newTabButton = new QToolButton(tabBarRow);
        m_newTabButton->setObjectName("newTabButton");
        m_newTabButton->setText(QStringLiteral("+"));
        m_newTabButton->setToolTip(tr("New Tab (Ctrl+T)"));
        m_newTabButton->setFixedSize(28, 28);
        m_newTabButton->setCursor(Qt::PointingHandCursor);
        rowLayout->addWidget(m_newTabButton, 0, Qt::AlignVCenter);
        ui->editorContainerLayout->insertWidget(qMax(0, barPos), tabBarRow);
        connect(m_newTabButton, &QToolButton::clicked, this, &MainWindow::showNewTabPanel);
    }
    QShortcut *shortcutNewTab = new QShortcut(QKeySequence("Ctrl+T"), this);
    connect(shortcutNewTab, &QShortcut::activated, this, &MainWindow::showNewTabPanel);
    editorStack->addWidget(ui->tabWidget);

    // Create unified scrollable settings page (tab added when user opens settings)
    unifiedSettingsWidget = createUnifiedSettingsWidget();
    editorStack->addWidget(unifiedSettingsWidget);

    // Welcome page is now a standalone pre-launch window (WelcomeMenuScreen)
    // shown in main.cpp before MainWindow is created.


    // Keep an empty icon bar at the drawer's foot for future quick actions
    QWidget *iconBar = new QWidget(ui->sidebarDrawer);
    iconBar->setObjectName("sidebarIconBar");
    QHBoxLayout *iconBarLayout = new QHBoxLayout(iconBar);
    iconBarLayout->setContentsMargins(6, 8, 6, 8);
    iconBarLayout->setSpacing(6);
    iconBarLayout->setAlignment(Qt::AlignCenter);
    ui->sidebarDrawerLayout->addWidget(iconBar);

    // Tab bar styling and connections
    tabBar->setTabsClosable(false);
    tabBar->installEventFilter(this);
    connect(tabBar, &QTabBar::currentChanged, this, &MainWindow::onTopTabChanged);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::updateStatusBar);
    // Keep the top tab bar in sync when the editor stack changes behind its
    // back (Save As, session restore, programmatic switches). Blocked
    // signals prevent ping-pong with onTopTabChanged.
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this]() {
        syncTopBarToCurrentFile();
    });

    // File tree
    connect(ui->fileTreeView, &QTreeView::clicked, this, &MainWindow::on_fileTreeView_clicked);

    // Tab close requests
    connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::on_tabWidget_tabCloseRequested);

    editorStack->addWidget(ui->tabWidget);

    if (!initialProject.isEmpty()) {
        loadProjectDirectory(initialProject);
        for (const QString &f : initialFiles) {
            if (QFile::exists(f))
                openFileInTab(f);
        }
    }
    showEditorInterface();

    connect(autoSaveTimer, &QTimer::timeout, this, &MainWindow::autoSave);
    // Idle-debounce auto-save: a single-shot timer re-armed on every text change
    // (wired in openFileInTab / on_fileTreeView_clicked / session hot-exit). It
    // fires ~2s after the user stops typing in any saved file.
    autoSaveTimer->setSingleShot(true);
    autoSaveTimer->setInterval(2000);
    autoSaveTimer->start();


    QShortcut *shortcutDialog = new QShortcut(QKeySequence("Ctrl+K"), this);
    connect(shortcutDialog, &QShortcut::activated, this, &MainWindow::showKeyboardShortcuts);

    QShortcut *shortcutCommandPalette = new QShortcut(QKeySequence("Ctrl+Shift+P"), this);
    connect(shortcutCommandPalette, &QShortcut::activated, this, &MainWindow::on_action_command_palette_triggered);

    // Session restore on startup (after editor is ready)
    if (m_sessionManager && m_sessionManager->hasSavedSession()) {
        QTimer::singleShot(500, this, [this]() {
            m_sessionManager->restoreSession();
        });
    }

    // Toggle folding: Ctrl+Shift+[
    QShortcut *shortcutFoldToggle = new QShortcut(QKeySequence("Ctrl+Shift+["), this);
    connect(shortcutFoldToggle, &QShortcut::activated, this, [this]() {
        CodeEditor *editor = getCurrentCodeEditor();
        if (editor && editor->foldManager()) {
            int line = editor->textCursor().blockNumber();
            editor->foldManager()->toggleFold(line);
        }
    });

    // Ctrl+W is documented as "Close Tab" — close the active tab; only fall
    // back to closing the window when there is nothing left to close.
    QShortcut *shortcutCloseTab = new QShortcut(QKeySequence("Ctrl+W"), this);
    connect(shortcutCloseTab, &QShortcut::activated, this, [this]() {
        int idx = ui->tabWidget->currentIndex();
        if (idx >= 0) {
            on_tabWidget_tabCloseRequested(idx);
            return;
        }
        // No file tabs — close the active settings/panel tab if one is open.
        int topIdx = tabBar->currentIndex();
        if (topIdx >= 0) {
            QVariant data = tabBar->tabData(topIdx);
            if (data.typeId() == QMetaType::Int) {
                for (int i = 0; i < tabBar->count(); ++i) {
                    if (tabBar->tabData(i).toInt() == static_cast<int>(TabType::Settings)) {
                        tabBar->removeTab(i);
                        break;
                    }
                }
                int cur = tabBar->currentIndex();
                if (cur < 0) {
                    showEditorInterface();
                } else {
                    QVariant curData = tabBar->tabData(cur);
                    bool isSettings = (curData.typeId() == QMetaType::Int
                                       && static_cast<TabType>(curData.toInt()) == TabType::Settings);
                    if (!isSettings)
                        showEditorInterface();
                }
            } else if (data.typeId() == QMetaType::QString) {
                QString strData = data.toString();
                if (strData.startsWith("panel:")) {
                    bool ok = false;
                    int panelIndex = strData.mid(6).toInt(&ok);
                    if (ok)
                        closePanelTab(panelIndex);
                }
            }
            updateTabBarVisibility();
            return;
        }
        close();
    });

    QShortcut *shortcutSave = new QShortcut(QKeySequence("Ctrl+S"), this);
    connect(shortcutSave, &QShortcut::activated, this, &MainWindow::on_action_save_triggered);

    QShortcut *shortcutSaveAs = new QShortcut(QKeySequence("Ctrl+Shift+S"), this);
    connect(shortcutSaveAs, &QShortcut::activated, this, &MainWindow::on_action_save_as_triggered);

    QShortcut *shortcutOpenProject = new QShortcut(QKeySequence("Ctrl+O"), this);
    connect(shortcutOpenProject, &QShortcut::activated, this, &MainWindow::on_action_open_project_triggered);


    // LSP response handlers (definition / declaration / typeDefinition / implementation / references)
    auto jumpToLocation = [this](const QJsonArray &locations) {
        if (locations.isEmpty())
            return;
        QJsonObject loc = locations.first().toObject();
        QString path = QUrl(loc["uri"].toString()).toLocalFile();
        QJsonObject start = loc["range"].toObject()["start"].toObject();
        int line = start["line"].toInt();
        int character = start["character"].toInt();
        if (!path.isEmpty() && path != currentFile)
            openFileInTab(path);
        CodeEditor *editor = getCurrentCodeEditor();
        if (!editor)
            return;
        QTextBlock block = editor->document()->findBlockByNumber(line);
        if (!block.isValid())
            return;
        QTextCursor cursor(editor->document());
        cursor.setPosition(block.position() + character);
        editor->setTextCursor(cursor);
        editor->centerCursor();
    };
    connect(lspClient, &RustLspClientAdapter::definitionReceived, this, jumpToLocation);
    // Note: declarationReceived, typeDefinitionReceived, implementationReceived are handled via separate requests
    // that return to the same handler pattern



    // Note: documentSymbol now uses the Rust adapter's completion callback pattern

    // Route LSP results (rename, code action, etc.) to the RefactoringManager
    connect(lspClient, &RustLspClientAdapter::completionReceived, m_refactoringManager, &RefactoringManager::onLspResultReceived);

    connect(lspClient, &RustLspClientAdapter::hoverReceived, this, [this](const QJsonObject &hover, int) {
        CodeEditor *editor = getCurrentCodeEditor();
        if (!editor)
            return;
        auto extractHover = [](const QJsonValue &v) -> QString {
            if (v.isString()) return v.toString();
            if (v.isObject()) {
                QJsonObject o = v.toObject();
                if (o.contains("value")) return o["value"].toString();
            }
            return QString();
        };
        QString text;
        QJsonValue cval = hover.value("contents");
        if (cval.isUndefined()) cval = QJsonValue(hover);
        if (cval.isArray()) {
            QStringList parts;
            for (const QJsonValue &e : cval.toArray()) {
                QString t = extractHover(e);
                if (!t.isEmpty()) parts << t;
            }
            text = parts.join("\n\n");
        } else {
            text = extractHover(cval);
        }
        text = text.trimmed();
        if (text.isEmpty())
            return;
        QToolTip::showText(editor->mapToGlobal(editor->cursorRect().bottomLeft()), text, editor);
    });



    // ── P0/P1/P2 Feature Modules ──────────────────────────────────────
    m_sessionManager = new SessionManager(this, ui->tabWidget, this);
    m_codeLensManager = new CodeLensManager(this);
    // When Code Lens items arrive, feed them to the matching editor
    connect(m_codeLensManager, &CodeLensManager::codeLensUpdated, this, [this](const QString &uri) {
        QString filePath = QUrl(uri).toLocalFile();
        for (int i = 0; i < ui->tabWidget->count(); ++i) {
            if (CodeEditor *ed = qobject_cast<CodeEditor*>(ui->tabWidget->widget(i))) {
                if (ed->filePath() == filePath) {
                    ed->setCodeLensItems(m_codeLensManager->itemsForDocument(uri));
                    break;
                }
            }
        }
    });
    m_statusBarWidget = new StatusBarWidget(this);
    m_encodingManager = new EncodingManager(this);
    m_notificationCenter = new NotificationCenter(this);

    // Replace default status bar with custom status bar
    statusBar()->setVisible(false);
    m_statusBarWidget->setObjectName("customStatusBar");
    statusBar()->addPermanentWidget(m_statusBarWidget);
    statusBar()->setVisible(true);
    m_statusBarWidget->setLanguage("Text");
    m_statusBarWidget->setEncoding("UTF-8");
    m_statusBarWidget->setLineEnding("LF");
    m_statusBarWidget->setIndentation("Spaces: 4");

    // Breadcrumb bar below tab bar
    m_breadcrumbBar = new BreadcrumbBarWidget(ui->editorContainer);
    ui->editorContainerLayout->insertWidget(1, m_breadcrumbBar);

    // Connect notification center to status bar
    connect(m_notificationCenter, &NotificationCenter::unreadCountChanged, this, [this](int count) {
        if (m_statusBarWidget) {
            m_statusBarWidget->setErrorCount(count, 0);
        }
    });

    // Connect session manager signals
    connect(m_sessionManager, &SessionManager::sessionRestored, this, [this]() {
        qDebug() << "Session restored";
    });
    // Open files requested by session restore
    connect(m_sessionManager, &SessionManager::sessionFileRequested, this,
            [this](const QString &filePath, int cursorLine, int cursorColumn, bool activate) {
        if (!QFile::exists(filePath)) return;
        openFileInTab(filePath);
        if (activate) {
            CodeEditor *editor = getCurrentCodeEditor();
            if (editor) {
                QTextBlock block = editor->document()->findBlockByNumber(cursorLine);
                if (block.isValid()) {
                    QTextCursor cursor(editor->document());
                    cursor.setPosition(block.position() + cursorColumn);
                    editor->setTextCursor(cursor);
                }
            }
        }
    });
    // Hot exit: restore unsaved buffers as untitled tabs
    connect(m_sessionManager, &SessionManager::hotExitFileRequested, this,
            [this](const QString &originalPath, const QString &content, int cursorLine, int cursorColumn) {
        CodeEditor *editor = new CodeEditor(this);
        editor->setFilePath(originalPath);
        if (!originalPath.isEmpty()) {
            editor->setLanguageForFile(originalPath);
        }
        editor->installEventFilter(this);
        QFont savedFont = SettingsStore::instance().value("editor/font", editor->font()).value<QFont>();
        editor->setFont(savedFont);
        editor->setTabWidth(SettingsStore::instance().value("editor/tabWidth", editor->tabWidth()).toInt());
        editor->setPlainText(content);
        editor->document()->setModified(true);
        QString displayName = originalPath.isEmpty() ? tr("Untitled") : QFileInfo(originalPath).fileName();
        // Dynamic index lookup — a captured index goes stale when other tabs close.
        QPointer<CodeEditor> hotEdGuard(editor);
        connect(editor, &QPlainTextEdit::modificationChanged, this,
                [this, hotEdGuard](bool m) {
                    if (!hotEdGuard)
                        return;
                    int i = ui->tabWidget->indexOf(hotEdGuard);
                    if (i >= 0)
                        updateTabModified(i, m);
                });
        connect(editor, &QPlainTextEdit::cursorPositionChanged, this, &MainWindow::updateStatusBar);
        // Re-arm the idle-debounce auto-save timer on every edit.
        connect(editor, &QPlainTextEdit::textChanged, this, [this]() {
            autoSaveTimer->start();
        });
        openFiles.append({originalPath, displayName, true});
        // Untitled buffers need a unique stable id — the empty path would
        // collide across tabs and break tab-bar lookups.
        QString hotId = originalPath.isEmpty()
            ? QStringLiteral("untitled:%1").arg(++m_untitledCounter)
            : originalPath;
        m_tabIds[editor] = hotId;
        showEditorInterface();
        ui->tabWidget->addTab(editor, "*" + displayName);
        int tabBarIndex = tabBar->addTab("*" + displayName);
        tabBar->setTabData(tabBarIndex, hotId);
        tabBar->setTabIcon(tabBarIndex, ThemeIcons::instance()->statefulIcon(
                                            FileIcons::forFileName(originalPath), 16));
        tabBar->setTabButton(tabBarIndex, QTabBar::RightSide, createEditorTabCloseButton(editor));
        ui->tabWidget->setCurrentWidget(editor);
        tabBar->setCurrentIndex(tabBarIndex);
        // Restore cursor position
        QTextBlock block = editor->document()->findBlockByNumber(cursorLine);
        if (block.isValid()) {
            QTextCursor cursor(editor->document());
            cursor.setPosition(block.position() + cursorColumn);
            editor->setTextCursor(cursor);
        }
    });

    // ── Status Bar dynamic updates ────────────────────────────────────────
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
        Q_UNUSED(index);
        updateStatusBar();
    });

    // Refactoring shortcuts
    QShortcut *shortcutRename = new QShortcut(QKeySequence("F2"), this);
    connect(shortcutRename, &QShortcut::activated, this, [this]() {
        CodeEditor *editor = getCurrentCodeEditor();
        if (editor && lspClient->isRunning() && !currentFile.isEmpty()) {
            m_refactoringManager->renameSymbol(editor, lspClient, currentFile);
        }
    });
    // Handle rename dialog request from RefactoringManager
    connect(m_refactoringManager, &RefactoringManager::renameDialogRequested, this,
            [this](const QString &symbol, const QString &filePath, int line, int column) {
        bool ok = false;
        QString newName = QInputDialog::getText(this, tr("Rename Symbol"),
                                                  tr("Rename '%1' to:").arg(symbol),
                                                  QLineEdit::Normal, symbol, &ok);
        if (ok && !newName.isEmpty() && newName != symbol) {
            CodeEditor *editor = getCurrentCodeEditor();
            if (editor) {
                m_refactoringManager->renameWithNewName(editor, lspClient, filePath, newName);
            }
        }
    });
    // Connect refactoring results
    connect(m_refactoringManager, &RefactoringManager::refactoringApplied, this, [this](int fileCount) {
        m_notificationCenter->addNotification(tr("Refactoring Complete"),
            tr("Applied changes to %1 file(s)").arg(fileCount), "success");
    });
    connect(m_refactoringManager, &RefactoringManager::refactoringFailed, this, [this](const QString &reason) {
        m_notificationCenter->addNotification(tr("Refactoring Failed"), reason, "error");
    });
    // Extract Method: Ctrl+Alt+M
    QShortcut *shortcutExtractMethod = new QShortcut(QKeySequence("Ctrl+Alt+M"), this);
    connect(shortcutExtractMethod, &QShortcut::activated, this, [this]() {
        CodeEditor *editor = getCurrentCodeEditor();
        if (editor && lspClient->isRunning()) {
            m_refactoringManager->extractMethod(editor, lspClient, currentFile);
        }
    });
    // Extract Variable: Ctrl+Alt+V
    QShortcut *shortcutExtractVariable = new QShortcut(QKeySequence("Ctrl+Alt+V"), this);
    connect(shortcutExtractVariable, &QShortcut::activated, this, [this]() {
        CodeEditor *editor = getCurrentCodeEditor();
        if (editor && lspClient->isRunning()) {
            m_refactoringManager->extractVariable(editor, lspClient, currentFile);
        }
    });

    // Config validator - validate settings on startup (Rust adapter handles this internally)

    // Initialize layout from settings. This also establishes the initial
    // sidebar width/collapse state, so no separate setSidebarCollapsed() call
    // is needed here — an animation started now would race the layout below.
    // Clamp the stored value so a corrupt or out-of-range entry can't produce
    // an unhandled switch branch.
    int storedLayout = SettingsStore::instance()
                           .value("layoutType", static_cast<int>(LayoutType::Classic)).toInt();
    if (storedLayout != static_cast<int>(LayoutType::Classic)) {
        storedLayout = static_cast<int>(LayoutType::Classic);
    }
    applyLayout(static_cast<LayoutType>(storedLayout));

    // Restore window geometry and state
    if (SettingsStore::instance().contains("mainWindow/geometry")) {
        restoreGeometry(SettingsStore::instance().value("mainWindow/geometry").toByteArray());
    }
    if (SettingsStore::instance().contains("mainWindow/state")) {
        restoreState(SettingsStore::instance().value("mainWindow/state").toByteArray());
    }
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::applyLayout(LayoutType layout)
{
    m_layoutType = layout;

    QHBoxLayout *mainLayout = qobject_cast<QHBoxLayout*>(ui->centralwidget->layout());
    if (!mainLayout) return;

    QWidget *sidebar = ui->sidebarDrawer;
    QWidget *editor = ui->editorContainer;
    if (!sidebar || !editor) return;

    // Detach the two top-level panes so they can be re-ordered in place.
    mainLayout->removeWidget(sidebar);
    mainLayout->removeWidget(editor);

    // A collapse animation still in flight would keep writing maximumWidth and
    // undo the width we are about to choose, so drop it first.
    cancelSidebarAnimations();
    {
        const bool collapsed =
            SettingsStore::instance().value("ui/sidebarCollapsed", true).toBool();
        const int width = collapsed ? 0 : DrawerMetrics::ExpandedWidth;
        sidebar->setMinimumWidth(width);
        sidebar->setMaximumWidth(width);
        m_sidebarCollapsed = collapsed;
        updateFileTreeFolderIcons();
    }

    mainLayout->addWidget(sidebar);
    mainLayout->addWidget(editor, 1);

    sidebar->show();
    editor->show();

    updateTabBarVisibility();

    mainLayout->activate();
    update();
}

QVariant FileTreeModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DecorationRole) {
        if (!index.isValid())
            return QVariant();
        // Folders lose their glyph while the drawer is open; files keep theirs.
        if (!m_folderIconsVisible && isDir(index))
            return QVariant();
        // Ask the provider directly instead of going through QFileSystemModel,
        // which stores each icon in per-item private data and never re-asks —
        // a theme change would otherwise leave stale colours on screen.
        // ThemeFileIconProvider memoises per (colour, dpr), so a repaint of the
        // visible rows costs a hash lookup, not a re-tint.
        //
        // The static_cast relies on the model only ever being given a
        // QFileIconProvider: MainWindow installs m_fileIconProvider (a
        // ThemeFileIconProvider) and nothing else calls setIconProvider.
        // QAbstractFileIconProvider is not a QObject, so qobject_cast cannot
        // express the check.
        if (const auto *provider = static_cast<const QFileIconProvider *>(iconProvider()))
            return provider->icon(fileInfo(index));
        return QVariant();
    }
    return QFileSystemModel::data(index, role);
}

void FileTreeModel::setFolderIconsVisible(bool visible)
{
    if (m_folderIconsVisible == visible)
        return;
    m_folderIconsVisible = visible;
    // The top-level rows are only the wake-up call — data() above re-answers for
    // every row the view repaints, so nested folders follow without a walk.
    const QModelIndex root = index(rootPath());
    const int rows = rowCount(root);
    if (rows > 0) {
        emit dataChanged(index(0, 0, root), index(rows - 1, 0, root),
                         { Qt::DecorationRole });
    }
}

void FileTreeModel::refreshDecorationRole()
{
    // Top-level rows are the wake-up call: QFileSystemModel re-queries the icon
    // provider per row as the view repaints, and ThemeFileIconProvider now
    // answers from its own memo, so nested folders follow without a walk.
    const QModelIndex root = index(rootPath());
    const int rows = rowCount(root);
    if (rows > 0) {
        emit dataChanged(index(0, 0, root), index(rows - 1, 0, root),
                         { Qt::DecorationRole });
    }
}

void MainWindow::updateFileTreeFolderIcons()
{
    if (!fileModel)
        return;
    // Drawer drawn out → the tree goes straight to the file list, no folder
    // glyphs; collapsed → the drawer is just a rail, so bring them back.
    fileModel->setFolderIconsVisible(m_sidebarCollapsed);
    // dataChanged only covers the top level; repaint the whole tree so rows the
    // view never re-lays-out drop their folder icon too.
    if (ui->fileTreeView)
        ui->fileTreeView->viewport()->update();
}

void MainWindow::cancelSidebarAnimations()
{
    for (const QPointer<QAbstractAnimation> &anim : m_sidebarAnims) {
        if (anim)
            anim->stop();
    }
    m_sidebarAnims.clear();
}

QPlainTextEdit* MainWindow::getCurrentEditor()
{
    return qobject_cast<QPlainTextEdit*>(ui->tabWidget->currentWidget());
}

CodeEditor* MainWindow::getCurrentCodeEditor()
{
    return qobject_cast<CodeEditor*>(ui->tabWidget->currentWidget());
}

int MainWindow::registerPanel(const QString &iconPath, const QString &tooltip,
                             const QString &title, bool builtin, QWidget *panelWidget)
{
    // Registration only: no tab is opened here. A panel is available, not open —
    // it gets its tab when it is picked from the tab bar's "+" menu (or by a
    // shortcut), so the window does not start covered in tabs nobody asked for.
    const int index = m_panels.size();
    PanelEntry entry;
    entry.panelIndex = index;
    entry.title = title;
    entry.tooltip = tooltip;
    entry.builtin = builtin;
    entry.panelWidget = panelWidget;
    m_panels.append(entry);
    m_panelIcons.insert(index, iconPath);

    if (m_newTabMenu)
        m_newTabMenuPanelCount = -1;

    return index;
}

int MainWindow::addPanelTab(int panelIndex)
{
    if (panelIndex < 0 || panelIndex >= m_panels.size())
        return -1;
    const PanelEntry &entry = m_panels.at(panelIndex);
    const int tabIndex = tabBar->addTab(entry.title);
    tabBar->setTabData(tabIndex, QString("panel:%1").arg(panelIndex));
    if (!entry.tooltip.isEmpty())
        tabBar->setTabToolTip(tabIndex, entry.tooltip);
    tabBar->setTabButton(tabIndex, QTabBar::RightSide, createTabCloseButton());
    updateTabIcon(QString("panel:%1").arg(panelIndex));
    return tabIndex;
}

void MainWindow::openPanelAsTab(int panelIndex)
{
    if (panelIndex < 0 || panelIndex >= m_panels.size())
        return;

    PanelEntry &entry = m_panels[panelIndex];
    QWidget *widget = entry.panelWidget;
    if (!widget)
        return;

    // The panel is a page of the content area, so it fills the tab completely.
    if (editorStack->indexOf(widget) < 0)
        editorStack->addWidget(widget);
    editorStack->setCurrentWidget(widget);

    int tabIndex = findPanelTabIndex(panelIndex);
    if (tabIndex < 0) {
        tabIndex = addPanelTab(panelIndex);
        if (m_newTabMenu)
            m_newTabMenuPanelCount = -1;
    }
    if (tabIndex >= 0 && tabBar->currentIndex() != tabIndex) {
        QSignalBlocker blocker(tabBar);
        tabBar->setCurrentIndex(tabIndex);
    }

    widget->setFocus();
    updateTabBarVisibility();
}

void MainWindow::showPanelIndex(int index)
{
    openPanelAsTab(index);
}

int MainWindow::findPanelIndex(const QString &title) const
{
    for (int i = 0; i < m_panels.size(); ++i) {
        if (m_panels[i].title == title)
            return i;
    }
    return -1;
}

int MainWindow::findPanelTabIndex(int panelIndex) const
{
    const QString panelId = QString("panel:%1").arg(panelIndex);
    for (int i = 0; i < tabBar->count(); ++i) {
        const QVariant data = tabBar->tabData(i);
        if (data.typeId() == QMetaType::QString && data.toString() == panelId)
            return i;
    }
    return -1;
}

void MainWindow::closePanelTab(int panelIndex)
{
    if (panelIndex < 0 || panelIndex >= m_panels.size())
        return;

    const int tabIdx = findPanelTabIndex(panelIndex);
    if (tabIdx >= 0) {
        QSignalBlocker blocker(tabBar);
        tabBar->removeTab(tabIdx);
    }

    // The widget stops being a page of the content area and is only hidden. The
    // registry entry stays, so the panel can be opened again from the "+" menu
    // exactly as it was left, and no later panel index shifts.
    QWidget *widget = m_panels[panelIndex].panelWidget;
    if (widget) {
        editorStack->removeWidget(widget);
        widget->hide();
    }

    if (m_newTabMenu)
        m_newTabMenuPanelCount = -1;

    if (tabIdx >= 0 && tabIdx < tabBar->count()) {
        // Someone else's tab is now on screen; make sure its page is shown too.
        onTopTabChanged(tabBar->currentIndex());
    }
    updateTabBarVisibility();
}

void MainWindow::removePanelTab(int panelIndex)
{
    // A dynamic panel's widget is its owner's to destroy; built-ins are
    // MainWindow's and stay alive (hidden) until the window goes away.
    if (panelIndex >= 0 && panelIndex < m_panels.size() && !m_panels[panelIndex].builtin) {
        QWidget *widget = m_panels[panelIndex].panelWidget;
        if (widget) {
            editorStack->removeWidget(widget);
            widget->hide();
            widget->deleteLater();
        }
    }
    if (panelIndex < 0 || panelIndex >= m_panels.size())
        return;

    closePanelTab(panelIndex);

    // The entry is gone for good, so every later panel shifted down by one:
    // move its icon with it and retarget its tab.
    m_panelIcons.remove(panelIndex);
    QMap<int, QString> shiftedIcons;
    for (auto it = m_panelIcons.constBegin(); it != m_panelIcons.constEnd(); ++it) {
        if (it.key() > panelIndex)
            shiftedIcons.insert(it.key() - 1, it.value());
    }
    m_panelIcons = shiftedIcons;

    m_panels.removeAt(panelIndex);
    for (int i = panelIndex; i < m_panels.size(); ++i) {
        m_panels[i].panelIndex = i;
        const int tIdx = findPanelTabIndex(i + 1);
        if (tIdx >= 0) {
            tabBar->setTabData(tIdx, QString("panel:%1").arg(i));
            updateTabIcon(QString("panel:%1").arg(i));
            tabBar->setTabToolTip(tIdx, m_panels[i].tooltip);
        }
    }
    if (m_newTabMenu)
        m_newTabMenuPanelCount = -1;
}

QPushButton* MainWindow::createTabCloseButton()
{
    QPushButton *closeBtn = new QPushButton();
    ThemeIcons::instance()->setIcon(closeBtn, ":/icons/close.svg");
    closeBtn->setFixedSize(20, 20);
    closeBtn->setFlat(true);
    closeBtn->setCursor(Qt::ArrowCursor);
    connect(closeBtn, &QPushButton::clicked, this, [this, closeBtn]() {
        // Find which tabBar tab owns this button, then read the CURRENT data
        // (not a stale capture — panel indices shift after close).
        for (int i = 0; i < tabBar->count(); ++i) {
            if (tabBar->tabButton(i, QTabBar::RightSide) == closeBtn) {
                QVariant data = tabBar->tabData(i);
                if (data.typeId() == QMetaType::QString) {
                    QString str = data.toString();
                    if (str.startsWith("panel:")) {
                        bool ok = false;
                        int panelIndex = str.mid(6).toInt(&ok);
                        if (ok)
                            closePanelTab(panelIndex);
                    } else {
                        // File tab — resolve by stable id
                        for (auto it = m_tabIds.constBegin(); it != m_tabIds.constEnd(); ++it) {
                            if (it.value() == str) {
                                int widx = ui->tabWidget->indexOf(it.key());
                                if (widx >= 0)
                                    on_tabWidget_tabCloseRequested(widx);
                                return;
                            }
                        }
                        for (int k = 0; k < openFiles.size(); ++k) {
                            if (openFiles[k].filePath == str) {
                                on_tabWidget_tabCloseRequested(k);
                                return;
                            }
                        }
                    }
                }
                return;
            }
        }
    });
    return closeBtn;
}


