// T08 外壳选型实验：Qt 6 Widgets 探针。与 Win32 基线同四个实验，结果写入 exe 同目录 qt-probe-results-*.txt。
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QUrl>
#include <QElapsedTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

static QPlainTextEdit* gLog = nullptr;
static QFile gFile;
static QElapsedTimer gStartTimer;

static void logLine(const QString& s) {
  QString line = QDateTime::currentDateTime().toString("[hh:mm:ss.zzz] ") + s;
  if (gFile.isOpen()) { gFile.write((line + "\r\n").toUtf8()); gFile.flush(); }
  if (gLog) gLog->appendPlainText(line);
}

// ---------- 实验 1：IME ----------
class ImeView : public QWidget {
 public:
  ImeView() {
    setWindowTitle("Qt 实验 1：IME 候选窗定位");
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    resize(760, 480);
    caret_ = QRect(120, 160, 2, 24);
    logLine("[Qt IME] 窗口打开。点击放光标，F2 随机，F3 右下角，F4 清空");
  }
 protected:
  QVariant inputMethodQuery(Qt::InputMethodQuery q) const override {
    if (q == Qt::ImCursorRectangle) return caret_;
    if (q == Qt::ImEnabled) return true;
    if (q == Qt::ImHints) return int(Qt::ImhNone);
    return QWidget::inputMethodQuery(q);
  }
  void inputMethodEvent(QInputMethodEvent* e) override {
    preedit_ = e->preeditString();
    if (!e->commitString().isEmpty()) {
      ++commits_; committed_ += e->commitString();
      logLine(QString("[Qt IME] 提交 #%1：「%2」").arg(commits_).arg(e->commitString()));
    }
    update(); e->accept();
  }
  void moveCaret(QPoint p) {
    caret_ = QRect(p, QSize(2, 24));
    QPoint g = mapToGlobal(caret_.topLeft());
    logLine(QString("[Qt IME] 光标矩形 客户区(%1,%2 %3×%4) 全局(%5,%6) DPR=%7")
                .arg(caret_.x()).arg(caret_.y()).arg(caret_.width()).arg(caret_.height())
                .arg(g.x()).arg(g.y()).arg(devicePixelRatioF()));
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    update();
  }
  void mousePressEvent(QMouseEvent* e) override { setFocus(); moveCaret(e->position().toPoint()); }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_F2) moveCaret(QPoint(QRandomGenerator::global()->bounded(qMax(1, width() - 40)), 60 + QRandomGenerator::global()->bounded(qMax(1, height() - 100))));
    else if (e->key() == Qt::Key_F3) moveCaret(QPoint(width() - 12, height() - 30));
    else if (e->key() == Qt::Key_F4) { committed_.clear(); commits_ = 0; update(); }
    else if (!e->text().isEmpty() && e->text()[0].isPrint()) { committed_ += e->text(); update(); }
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.drawText(QRect(8, 8, width() - 16, 48), Qt::TextWordWrap, "点击放置模拟光标（红框）；F2 随机，F3 右下角，F4 清空。看：候选窗是否贴在红框下方且不遮挡。");
    p.fillRect(caret_, Qt::red);
    p.setPen(QColor(0, 90, 200)); p.drawText(caret_.right() + 4, caret_.bottom() - 4, preedit_);
    p.setPen(Qt::black); p.drawText(8, height() - 12, QString("已提交 %1 次：%2").arg(commits_).arg(committed_));
  }
 private:
  QRect caret_; QString preedit_, committed_; int commits_ = 0;
};

// ---------- 实验 3：分屏 ----------
class Pane : public QWidget {
 public:
  explicit Pane(QString n, QColor c) : name_(std::move(n)), color_(c) {}
  QFont termFont() const { QFont f("Consolas"); f.setPointSize(gPt); f.setStyleHint(QFont::Monospace); return f; }
  QSize cell() const { QFontMetrics fm(termFont()); return {fm.horizontalAdvance('M'), fm.height()}; }
  QSize minimumSizeHint() const override { auto c = cell(); return {20 * c.width(), 5 * c.height()}; }  // 20×5
  QString report() const { auto c = cell(); return QString("%1=%2×%3").arg(name_).arg(width() / c.width()).arg(height() / c.height()); }
  bool ok() const { auto c = cell(); return width() / c.width() >= 20 && height() / c.height() >= 5; }
  static inline int gPt = 12;
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.fillRect(rect(), color_);
    auto c = cell();
    p.setPen(ok() ? QColor(120, 230, 120) : QColor(255, 90, 90));
    p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop,
               QString("窗格 %1\n%2 列 × %3 行 %4").arg(name_).arg(width() / c.width()).arg(height() / c.height()).arg(ok() ? "✔" : "✘ 低于 20×5"));
  }
 private:
  QString name_; QColor color_;
};

class SplitView : public QWidget {
 public:
  SplitView() {
    setWindowTitle("Qt 实验 3：分屏 20×5（QSplitter + minimumSizeHint）");
    resize(900, 560);
    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel("拖动分隔条到极限；缩小窗口；按 +/- 改字号。看：每个窗格是否始终 ≥ 20×5。"));
    a_ = new Pane("A", QColor(30, 30, 30)); b_ = new Pane("B", QColor(20, 40, 70)); c_ = new Pane("C", QColor(60, 30, 30));
    auto* right = new QSplitter(Qt::Vertical); right->addWidget(b_); right->addWidget(c_);
    auto* root = new QSplitter(Qt::Horizontal); root->addWidget(a_); root->addWidget(right);
    for (auto* s : {root, right}) { s->setChildrenCollapsible(false); connect(s, &QSplitter::splitterMoved, this, [this] { dirty_ = true; }); }
    lay->addWidget(root, 1);
    setFocusPolicy(Qt::StrongFocus);
    logLine("===== Qt 实验 3：分屏 20×5 =====");
  }
  void reportNow(const QString& why) {
    auto c = a_->cell();
    bool bad = !a_->ok() || !b_->ok() || !c_->ok();
    if (bad) ++violations_;
    logLine(QString("[Qt 分屏] %1：cell %2×%3（Consolas %4pt）%5 %6 %7%8").arg(why).arg(c.width()).arg(c.height()).arg(Pane::gPt)
                .arg(a_->report(), b_->report(), c_->report(), bad ? "  ← 低于 20×5" : ""));
  }
 protected:
  void mouseReleaseEvent(QMouseEvent*) override {}
  bool event(QEvent* e) override {
    if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::NonClientAreaMouseButtonRelease || (e->type() == QEvent::Enter && dirty_)) {
      if (dirty_) { dirty_ = false; reportNow("拖动/缩放后"); }
    }
    return QWidget::event(e);
  }
  void resizeEvent(QResizeEvent*) override { dirty_ = true; }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) Pane::gPt = qMin(72, Pane::gPt + 2);
    else if (e->key() == Qt::Key_Minus) Pane::gPt = qMax(6, Pane::gPt - 2);
    else if (e->key() == Qt::Key_R) { reportNow("手动报告"); return; }
    else return;
    for (auto* p : {a_, b_, c_}) { p->updateGeometry(); p->update(); }
    reportNow("字号变化");
  }
  void closeEvent(QCloseEvent*) override { reportNow("关闭前"); logLine(QString("[Qt 分屏] 低于 20×5 次数：%1").arg(violations_)); }
 private:
  Pane *a_, *b_, *c_; bool dirty_ = false; int violations_ = 0;
};

// ---------- 实验 4：快捷键 ----------
class KeysView : public QWidget {
 public:
  KeysView() {
    setWindowTitle("Qt 实验 4：快捷键捕获");
    resize(620, 440); setFocusPolicy(Qt::StrongFocus);
    names_ = {"Ctrl+Tab", "Ctrl+Shift+Tab", "Ctrl+Shift+T", "Alt+←", "Alt+→", "Alt+↑", "Alt+↓",
              "F10（不应激活菜单）", "Alt 单独按下松开", "Win+←（系统保留，预期收不到）", "Win 单独（系统保留）"};
    hit_.assign(names_.size(), false);
    logLine("===== Qt 实验 4：快捷键捕获 =====");
  }
 protected:
  bool event(QEvent* e) override {
    // Qt 会把 Tab/Backtab 用于焦点切换：在 event() 里截取，并记录 ShortcutOverride
    if (e->type() == QEvent::ShortcutOverride) { e->accept(); }
    if (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
      auto* k = static_cast<QKeyEvent*>(e);
      bool down = e->type() == QEvent::KeyPress;
      logLine(QString("[Qt 快捷键] %1 key=0x%2 mods=0x%3 nativeVK=0x%4 scan=0x%5 auto=%6 text=%7")
                  .arg(down ? "KeyPress" : "KeyRelease").arg(k->key(), 0, 16).arg(int(k->modifiers()), 0, 16)
                  .arg(k->nativeVirtualKey(), 0, 16).arg(k->nativeScanCode(), 0, 16).arg(k->isAutoRepeat()).arg(k->text().toHtmlEscaped()));
      auto m = k->modifiers(); bool ctrl = m & Qt::ControlModifier, shift = m & Qt::ShiftModifier, alt = m & Qt::AltModifier, win = m & Qt::MetaModifier;
      if (down) {
        altAlone_ = k->key() == Qt::Key_Alt ? !k->isAutoRepeat() || altAlone_ : false;
        if (k->key() == Qt::Key_Tab && ctrl) hit(0);
        if (k->key() == Qt::Key_Backtab && ctrl) hit(1);
        if (k->key() == Qt::Key_T && ctrl && shift) hit(2);
        if (alt && !ctrl) { if (k->key() == Qt::Key_Left) hit(3); if (k->key() == Qt::Key_Right) hit(4); if (k->key() == Qt::Key_Up) hit(5); if (k->key() == Qt::Key_Down) hit(6); }
        if (k->key() == Qt::Key_F10) hit(7);
        if (win && k->key() == Qt::Key_Left) hit(9);
        if (k->key() == Qt::Key_Meta) hit(10);
      } else if (k->key() == Qt::Key_Alt && altAlone_) { altAlone_ = false; hit(8); }
      return true;
    }
    if (e->type() == QEvent::FocusOut) logLine("[Qt 快捷键] 失去焦点（刚按的组合可能被系统截走）");
    return QWidget::event(e);
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); int y = 20;
    p.drawText(10, y, "保持本窗口在前台依次按下列组合。✔ = 收到。"); y += 30;
    for (size_t i = 0; i < names_.size(); ++i) {
      p.setPen(hit_[i] ? QColor(0, 140, 0) : QColor(90, 90, 90));
      p.drawText(16, y, QString(hit_[i] ? "✔  " : "☐  ") + names_[i]); y += 24;
    }
  }
  void closeEvent(QCloseEvent*) override {
    QStringList miss; int n = 0;
    for (size_t i = 0; i < names_.size(); ++i) { if (hit_[i]) ++n; else miss << names_[i]; }
    logLine(QString("[Qt 快捷键] 捕获 %1/%2；未捕获：%3").arg(n).arg(names_.size()).arg(miss.isEmpty() ? "无" : miss.join("，")));
  }
 private:
  void hit(int i) { if (!hit_[i]) { hit_[i] = true; logLine("[Qt 快捷键] ✔ " + names_[i]); update(); } }
  QStringList names_; std::vector<bool> hit_; bool altAlone_ = false;
};

// ---------- 实验 2：便携 ----------
static void portableReport() {
  logLine("===== Qt 实验 2：便携启动 =====");
  logLine("[Qt 便携] exe：" + QCoreApplication::applicationFilePath());
  logLine(QString("[Qt 便携] 启动到主窗口：%1 ms（QApplication 构造起算；进程创建起算见下）").arg(gStartTimer.elapsed()));
  logLine(QString("[Qt 便携] Qt %1，平台插件 %2，插件目录 %3").arg(qVersion(), QGuiApplication::platformName(), QLibraryInfo::path(QLibraryInfo::PluginsPath)));
  if (auto* s = QGuiApplication::primaryScreen()) logLine(QString("[Qt 便携] 主屏 DPR=%1 逻辑 DPI=%2").arg(s->devicePixelRatio()).arg(s->logicalDotsPerInch()));
  QFile t(QCoreApplication::applicationDirPath() + "/.write-test.tmp");
  bool w = t.open(QIODevice::WriteOnly); t.remove();
  logLine(QString("[Qt 便携] exe 目录可写：%1").arg(w ? "是" : "否"));
#ifdef Q_OS_WIN
  FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); FILETIME now; GetSystemTimeAsFileTime(&now);
  ULONGLONG d = (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - (((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime);
  logLine(QString("[Qt 便携] 进程创建到现在：%1 ms").arg(d / 10000));
  auto aw = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
  logLine(QString("[Qt 便携] DPI 感知=%1（2=Per-Monitor）").arg(int(aw)));
  wchar_t wd[MAX_PATH]; GetWindowsDirectoryW(wd, MAX_PATH); QString winDir = QString::fromWCharArray(wd).toLower();
  HMODULE mods[1024]; DWORD need = 0; int non = 0; qint64 bytes = 0;
  if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
    for (DWORD i = 1; i < need / sizeof(HMODULE); ++i) {
      wchar_t mp[MAX_PATH * 2]; GetModuleFileNameW(mods[i], mp, MAX_PATH * 2);
      QString p = QString::fromWCharArray(mp);
      if (!p.toLower().startsWith(winDir)) { ++non; bytes += QFileInfo(p).size(); logLine("[Qt 便携]   随包 DLL：" + p); }
    }
  }
  logLine(QString("[Qt 便携] 非系统 DLL %1 个，共 %2 MB（便携包需要随带）").arg(non).arg(bytes / 1048576.0, 0, 'f', 1));
#endif
}

int main(int argc, char** argv) {
  gStartTimer.start();
  QApplication app(argc, argv);
  QString path = QCoreApplication::applicationDirPath() + "/qt-probe-results-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".txt";
  gFile.setFileName(path);
  if (!gFile.open(QIODevice::WriteOnly)) { path = QDir::tempPath() + "/" + QFileInfo(path).fileName(); gFile.setFileName(path); gFile.open(QIODevice::WriteOnly); }
  gFile.write("\xEF\xBB\xBF");

  QWidget main; main.setWindowTitle("it2w 外壳选型探针（Qt 6 Widgets）"); main.resize(980, 640);
  auto* v = new QVBoxLayout(&main); auto* row = new QHBoxLayout; v->addLayout(row);
  auto* b1 = new QPushButton("1 IME 候选窗"); auto* b2 = new QPushButton("2 便携启动信息");
  auto* b3 = new QPushButton("3 分屏 20×5"); auto* b4 = new QPushButton("4 快捷键捕获");
  for (auto* b : {b1, b2, b3, b4}) row->addWidget(b);
  auto* noteRow = new QHBoxLayout; v->addLayout(noteRow);
  auto* note = new QLineEdit; note->setPlaceholderText("观察结论 → 写入结果文件"); noteRow->addWidget(note, 1);
  auto* bn = new QPushButton("写入结论"); auto* bo = new QPushButton("打开结果文件"); noteRow->addWidget(bn); noteRow->addWidget(bo);
  gLog = new QPlainTextEdit; gLog->setReadOnly(true); v->addWidget(gLog, 1);
  QObject::connect(b1, &QPushButton::clicked, [] { auto* w = new ImeView; w->setAttribute(Qt::WA_DeleteOnClose); w->show(); w->setFocus(); });
  QObject::connect(b2, &QPushButton::clicked, [] { portableReport(); });
  QObject::connect(b3, &QPushButton::clicked, [] { auto* w = new SplitView; w->setAttribute(Qt::WA_DeleteOnClose); w->show(); w->setFocus(); });
  QObject::connect(b4, &QPushButton::clicked, [] { auto* w = new KeysView; w->setAttribute(Qt::WA_DeleteOnClose); w->show(); w->activateWindow(); w->setFocus(); });
  auto write = [note] { if (!note->text().isEmpty()) { logLine("[结论] " + note->text()); note->clear(); } };
  QObject::connect(bn, &QPushButton::clicked, write);
  QObject::connect(note, &QLineEdit::returnPressed, write);
  QObject::connect(bo, &QPushButton::clicked, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
  main.show();
  logLine("结果文件：" + path);
  portableReport();
  return app.exec();
}
