#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QColor>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QMainWindow>
#include <QMouseEvent>
#include <QMimeData>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QToolButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QAbstractItemView>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QRect>

#include "reed/adb.hpp"
#include "reed/config.hpp"
#include "reed/device.hpp"
#include "reed/panel.hpp"
#include "reed/media.hpp"

namespace fs = std::filesystem;

namespace {

struct CommandResult {
  int exit_code = -1;
  QString output;
  bool timed_out = false;
};

struct CropSettings {
  double cx = 0.5;     // 0..1
  double cy = 0.5;     // 0..1
  double scale = 1.0;  // 0.35..1.0
};

CommandResult run_command(const QString& program, const QStringList& args,
                          int timeout_ms = 15000) {
  QProcess process;
  process.start(program, args);
  const bool finished = process.waitForFinished(timeout_ms);
  CommandResult result;
  if (!finished) {
    result.timed_out = true;
    process.kill();
    process.waitForFinished();
    result.exit_code = -1;
  } else {
    result.exit_code = process.exitCode();
  }
  result.output = QString::fromUtf8(process.readAllStandardOutput()) +
                  QString::fromUtf8(process.readAllStandardError());
  if (result.timed_out) {
    result.output +=
        "\nCommand timed out after " + QString::number(timeout_ms) + "ms.\n";
  }
  return result;
}

QString join_strings(const std::vector<std::string>& values) {
  QStringList out;
  for (const auto& value : values) out << QString::fromStdString(value);
  return out.join(", ");
}

QString human_size_kb(uint64_t kb) {
  const double bytes = static_cast<double>(kb) * 1024.0;
  const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  int idx = 0;
  double v = bytes;
  while (v >= 1024.0 && idx < 4) {
    v /= 1024.0;
    ++idx;
  }
  return QString::number(v, 'f', idx < 2 ? 0 : 1) + " " + suffixes[idx];
}

bool is_generated_split_media(const std::string& name) {
  return name.rfind("split_", 0) == 0 && name.size() > 10 &&
         name.substr(name.size() - 4) == ".mp4";
}

/** Composed fullscreen GIF/image MP4s (.bgmix_* hidden from normal ls; bgmix_* legacy). */
bool is_generated_bgmix_media(const std::string& name) {
  if (name.size() < 11 || name.substr(name.size() - 4) != ".mp4") return false;
  if (name.rfind(".bgmix_", 0) == 0) return true;
  if (name.rfind("bgmix_", 0) == 0) return true;
  return false;
}

struct SplitComposeResult {
  std::string remote_mp4;
};

std::optional<double> probe_duration_seconds(const QString& path) {
  CommandResult result = run_command(
      "ffprobe",
      {"-v", "error", "-show_entries", "format=duration",
       "-of", "default=noprint_wrappers=1:nokey=1", path});
  if (result.exit_code != 0) return std::nullopt;
  bool ok = false;
  const double duration = result.output.trimmed().toDouble(&ok);
  if (!ok || duration <= 0.0) return std::nullopt;
  return duration;
}

// ffprobe on some GIFs reports absurd format durations (hours); caps keep Apply from
// running multi-hour ffmpeg jobs that hit QProcess timeouts.
constexpr double kMaxComposeDurationSec = 300.0;

/** QListWidget drags may report the viewport widget as QDrag::source(). */
static QListWidget* drag_source_as_list_widget(QObject* src) {
  if (!src) return nullptr;
  if (auto* lw = qobject_cast<QListWidget*>(src)) return lw;
  if (auto* w = qobject_cast<QWidget*>(src)) {
    return qobject_cast<QListWidget*>(w->parentWidget());
  }
  return nullptr;
}

double clamp_compose_duration(double seconds, const QString& label,
                              std::function<void(const QString&)> log_line) {
  if (!std::isfinite(seconds) || seconds <= 0.0) {
    return 12.0;
  }
  if (seconds > kMaxComposeDurationSec) {
    log_line(QString("WARNING: %1 reported duration %2s — capping to %3s (bad GIF/metadata?)")
                 .arg(label)
                 .arg(seconds, 0, 'f', 1)
                 .arg(kMaxComposeDurationSec, 0, 'f', 0));
    return kMaxComposeDurationSec;
  }
  return seconds;
}

/** Device media grid: drag files from the file manager to upload; drag items to slots. */
class RemoteMediaListWidget : public QListWidget {
 public:
  explicit RemoteMediaListWidget(QWidget* parent = nullptr) : QListWidget(parent) {
    setAcceptDrops(true);
  }

  std::function<void(const QStringList&)> on_files_dropped;

 protected:
  void dragEnterEvent(QDragEnterEvent* event) override {
    if (event->mimeData()->hasUrls()) {
      event->acceptProposedAction();
      return;
    }
    QListWidget::dragEnterEvent(event);
  }

  void dragMoveEvent(QDragMoveEvent* event) override {
    if (event->mimeData()->hasUrls()) {
      event->acceptProposedAction();
      return;
    }
    QListWidget::dragMoveEvent(event);
  }

  void dropEvent(QDropEvent* event) override {
    if (event->mimeData()->hasUrls()) {
      QStringList files;
      for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) files << url.toLocalFile();
      }
      if (!files.isEmpty() && on_files_dropped) on_files_dropped(files);
      event->acceptProposedAction();
      return;
    }
    QListWidget::dropEvent(event);
  }
};

class CropPreviewWidget : public QWidget {
 public:
  /** Draggable crop region: 1:1 or 2:1 (fullscreen) in the preview. */
  enum class CropShape { k1x1, k2x1 };

  explicit CropPreviewWidget(CropShape shape = CropShape::k1x1, QWidget* parent = nullptr)
      : QWidget(parent), crop_shape_(shape) {
    setMinimumHeight(90);
    setMouseTracking(true);
  }

  void set_pixmap(const QPixmap& pixmap) {
    pixmap_ = pixmap;
    update();
  }

  CropSettings settings() const { return settings_; }
  void set_settings(const CropSettings& settings) {
    settings_ = settings;
    clamp_settings();
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(47, 47, 47));
    if (pixmap_.isNull()) {
      p.setPen(QColor(180, 180, 180));
      p.drawText(rect(), Qt::AlignCenter, "Drop media here");
      return;
    }

    const QRect target = draw_rect();
    p.drawPixmap(target, pixmap_);

    const QRect crop = crop_rect(target);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(80, 220, 255), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(crop);
  }

  void mousePressEvent(QMouseEvent* event) override {
    const QRect target = draw_rect();
    const QRect crop = crop_rect(target);
    dragging_ = crop.contains(event->pos());
    if (dragging_) {
      drag_anchor_ = event->pos();
      start_settings_ = settings_;
    }
    QWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!dragging_ || pixmap_.isNull()) {
      QWidget::mouseMoveEvent(event);
      return;
    }
    const QRect target = draw_rect();
    if (!target.isValid()) return;
    const QSize crop_sz = crop_size_for_target(target);
    const int dx = event->pos().x() - drag_anchor_.x();
    const int dy = event->pos().y() - drag_anchor_.y();
    const double x_range = std::max(1, target.width() - crop_sz.width());
    const double y_range = std::max(1, target.height() - crop_sz.height());
    settings_.cx = start_settings_.cx + static_cast<double>(dx) / x_range;
    settings_.cy = start_settings_.cy + static_cast<double>(dy) / y_range;
    clamp_settings();
    update();
    QWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    dragging_ = false;
    QWidget::mouseReleaseEvent(event);
  }

  void wheelEvent(QWheelEvent* event) override {
    // Zoom crop box size in/out.
    const int delta = event->angleDelta().y();
    if (delta != 0) {
      settings_.scale += delta > 0 ? -0.05 : 0.05;
      clamp_settings();
      update();
    }
    QWidget::wheelEvent(event);
  }

 private:
  CropShape crop_shape_ = CropShape::k1x1;
  QPixmap pixmap_;
  CropSettings settings_;
  bool dragging_ = false;
  QPoint drag_anchor_;
  CropSettings start_settings_;

  QRect draw_rect() const {
    if (pixmap_.isNull()) return QRect();
    const QSize fit = pixmap_.size().scaled(size(), Qt::KeepAspectRatio);
    const int x = (width() - fit.width()) / 2;
    const int y = (height() - fit.height()) / 2;
    return QRect(x, y, fit.width(), fit.height());
  }

  QSize crop_size_for_target(const QRect& target) const {
    if (crop_shape_ == CropShape::k1x1) {
      const int min_dim = std::min(target.width(), target.height());
      const int s = std::max(1, static_cast<int>(min_dim * settings_.scale));
      return QSize(s, s);
    }
    // 2:1 box (width = 2 * height) inside target
    const int max_h = std::max(1, std::min(target.height(), target.width() / 2));
    int h = std::max(1, static_cast<int>(max_h * settings_.scale));
    int w = 2 * h;
    if (w > target.width()) {
      w = target.width() - (target.width() % 2);
      h = std::max(1, w / 2);
    }
    if (h > target.height()) {
      h = target.height();
      w = std::min(target.width(), 2 * h);
    }
    return QSize(w, h);
  }

  QRect crop_rect(const QRect& target) const {
    if (!target.isValid()) return QRect();
    const QSize sz = crop_size_for_target(target);
    const int x =
        target.x() + static_cast<int>((target.width() - sz.width()) * settings_.cx);
    const int y =
        target.y() + static_cast<int>((target.height() - sz.height()) * settings_.cy);
    return QRect(x, y, sz.width(), sz.height());
  }

  void clamp_settings() {
    settings_.scale = std::max(0.35, std::min(1.0, settings_.scale));
    settings_.cx = std::max(0.0, std::min(1.0, settings_.cx));
    settings_.cy = std::max(0.0, std::min(1.0, settings_.cy));
  }
};

class MediaDropTarget : public QFrame {
 public:
  /**
   * @param fixed_preview If valid, the "Drop media here" preview keeps this exact size
   *                      (1:1 or 2:1 matching crop_shape).
   */
  explicit MediaDropTarget(
      const QString& title,
      CropPreviewWidget::CropShape crop_shape = CropPreviewWidget::CropShape::k1x1,
      QSize fixed_preview = QSize(),
      QWidget* parent = nullptr)
      : QFrame(parent) {
    setAcceptDrops(true);
    setFrameShape(QFrame::StyledPanel);
    setLineWidth(2);

    auto* layout = new QVBoxLayout(this);
    title_label_ = new QLabel(title);
    crop_preview_ = new CropPreviewWidget(crop_shape);
    crop_preview_->setStyleSheet("QWidget { border: 1px solid #555; }");
    if (fixed_preview.isValid()) {
      crop_preview_->setFixedSize(fixed_preview);
    } else {
      crop_preview_->setMinimumHeight(90);
      setMinimumSize(220, 150);
    }
    name_label_ = new QLabel("<empty>");
    name_label_->setAlignment(Qt::AlignCenter);
    name_label_->setWordWrap(true);
    title_label_->setAlignment(Qt::AlignCenter);

    clear_btn_ = new QToolButton(this);
    clear_btn_->setText(QString::fromUtf8("×"));
    clear_btn_->setToolTip("Clear this slot");
    clear_btn_->setFixedSize(18, 18);
    clear_btn_->setCursor(Qt::PointingHandCursor);
    clear_btn_->setStyleSheet(
        "QToolButton {"
        "  background-color: #c53f3f;"
        "  color: white;"
        "  border: 1px solid #8f2f2f;"
        "  border-radius: 9px;"
        "  font-weight: bold;"
        "  font-size: 12px;"
        "  padding: 0px;"
        "}"
        "QToolButton:hover { background-color: #d94f4f; }"
        "QToolButton:pressed { background-color: #aa3434; }");
    clear_btn_->hide();
    connect(clear_btn_, &QToolButton::clicked, this, [this]() {
      if (on_clear_requested) on_clear_requested();
    });

    const int preview_stretch = fixed_preview.isValid() ? 0 : 1;
    layout->addWidget(title_label_, 0, Qt::AlignHCenter);
    layout->addWidget(crop_preview_, preview_stretch, Qt::AlignHCenter);
    layout->addWidget(name_label_, 0, Qt::AlignHCenter);
  }

  std::function<void(const QString&)> on_media_dropped;
  /** Called when a list item drag enters this target (before drop completes). */
  std::function<void()> on_drag_enter_from_list;
  std::function<void()> on_clear_requested;

  void set_media(const QString& media_name, const QIcon& icon) {
    media_name_ = media_name;
    name_label_->setText(media_name.isEmpty() ? "<empty>" : media_name);
    const QSize psz = crop_preview_->size();
    crop_preview_->set_pixmap(
        psz.isValid() && psz.width() > 0 && psz.height() > 0
            ? icon.pixmap(psz)
            : icon.pixmap(220, 140));
    update_clear_button_visibility();
  }

  QString media_name() const { return media_name_; }
  CropSettings crop_settings() const { return crop_preview_->settings(); }
  void set_crop_settings(const CropSettings& settings) {
    crop_preview_->set_settings(settings);
  }

 protected:
  void enterEvent(QEnterEvent* event) override {
    update_clear_button_visibility();
    QFrame::enterEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    clear_btn_->hide();
    QFrame::leaveEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QFrame::resizeEvent(event);
    if (clear_btn_) clear_btn_->move(width() - clear_btn_->width() - 6, 6);
  }

  void dragEnterEvent(QDragEnterEvent* event) override {
    if (drag_source_as_list_widget(event->source())) {
      if (on_drag_enter_from_list) on_drag_enter_from_list();
      event->acceptProposedAction();
      return;
    }
    QFrame::dragEnterEvent(event);
  }

  void dropEvent(QDropEvent* event) override {
    auto* source = drag_source_as_list_widget(event->source());
    if (!source) {
      QFrame::dropEvent(event);
      return;
    }
    QListWidgetItem* item = source->currentItem();
    if (!item) {
      event->ignore();
      return;
    }
    QString media = item->data(Qt::UserRole).toString().trimmed();
    if (media.isEmpty()) media = item->text().trimmed();
    if (!media.isEmpty() && on_media_dropped) on_media_dropped(media);
    event->acceptProposedAction();
  }

 private:
  void update_clear_button_visibility() {
    if (!clear_btn_) return;
    const bool show = underMouse() && !media_name_.isEmpty();
    clear_btn_->setVisible(show);
  }

  QLabel* title_label_ = nullptr;
  CropPreviewWidget* crop_preview_ = nullptr;
  QLabel* name_label_ = nullptr;
  QToolButton* clear_btn_ = nullptr;
  QString media_name_;
};

/**
 * Map UI crop sliders (same math as CropPreviewWidget) to a pixel rectangle on
 * the source frame for ffmpeg crop=w:h:x:y.
 * @param use_2x1_shape false: square crop (1:1), true: 2:1 fullscreen crop box.
 */
std::optional<QRect> ffmpeg_crop_rect_pixels(const QSize& src, const CropSettings& crop,
                                              bool use_2x1_shape) {
  if (!src.isValid() || src.width() < 2 || src.height() < 2) return std::nullopt;
  const int W = src.width();
  const int H = src.height();
  int crop_w = 0;
  int crop_h = 0;
  if (!use_2x1_shape) {
    const int min_dim = std::min(W, H);
    const int s = std::max(32, static_cast<int>(std::round(min_dim * crop.scale)));
    crop_w = crop_h = s;
  } else {
    const int max_h = std::max(1, std::min(H, W / 2));
    int h = std::max(1, static_cast<int>(max_h * crop.scale));
    int w = 2 * h;
    if (w > W) {
      w = W - (W % 2);
      if (w < 2) w = 2;
      h = std::max(1, w / 2);
    }
    if (h > H) {
      h = H;
      w = std::min(W, 2 * h);
    }
    crop_w = w;
    crop_h = h;
  }
  const int max_x = std::max(0, W - crop_w);
  const int max_y = std::max(0, H - crop_h);
  const int x = std::clamp(static_cast<int>(std::round(max_x * crop.cx)), 0, max_x);
  const int y = std::clamp(static_cast<int>(std::round(max_y * crop.cy)), 0, max_y);
  return QRect(x, y, crop_w, crop_h);
}

class MainWindow : public QMainWindow {
 public:
  MainWindow() {
    setWindowTitle("reed-tpse GUI");
    setMinimumSize(1040, 720);
    resize(1040, 820);

    const QString base_cache =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    media_cache_dir_ = base_cache + "/media";
    thumb_cache_dir_ = base_cache + "/thumbs";
    fs::create_directories(media_cache_dir_.toStdString());
    fs::create_directories(thumb_cache_dir_.toStdString());

    auto* central = new QWidget(this);
    central_panel_ = central;
    auto* root_layout = new QVBoxLayout(central);

    advanced_settings_host_ = new QWidget(central);
    auto* adv_layout = new QVBoxLayout(advanced_settings_host_);
    log_box_ = new QPlainTextEdit(advanced_settings_host_);
    log_box_->setReadOnly(true);
    log_box_->setPlaceholderText("Operation log…");
    log_box_->setMinimumHeight(200);
    adv_layout->addWidget(build_health_group(advanced_settings_host_));
    adv_layout->addWidget(build_device_group(advanced_settings_host_));
    adv_layout->addWidget(build_settings_utilities_group(advanced_settings_host_));
    adv_layout->addWidget(build_service_group(advanced_settings_host_));
    adv_layout->addWidget(log_box_, 1);
    advanced_settings_host_->hide();

    root_layout->addWidget(build_media_group());
    root_layout->addWidget(build_playback_group(), 1);

    setCentralWidget(central);

    log_line("[media cache] " + QDir::toNativeSeparators(media_cache_dir_));
    log_line("[thumb cache] " + QDir::toNativeSeparators(thumb_cache_dir_));

    load_config();
    refresh_devices();
    refresh_health();
    refresh_remote_media();
    refresh_service_status();
  }

 private:
  QPlainTextEdit* log_box_ = nullptr;
  QWidget* central_panel_ = nullptr;
  QWidget* advanced_settings_host_ = nullptr;
  QLabel* group_status_ = nullptr;
  QLabel* adb_status_ = nullptr;
  QLabel* serial_status_ = nullptr;
  QLabel* service_status_ = nullptr;
  QLabel* storage_status_ = nullptr;

  QComboBox* device_combo_ = nullptr;
  QLineEdit* custom_port_edit_ = nullptr;

  RemoteMediaListWidget* remote_media_list_ = nullptr;

  QString media_cache_dir_;
  QString thumb_cache_dir_;

  enum class CanvasMode { Fullscreen, Split };
  CanvasMode active_mode_ = CanvasMode::Fullscreen;
  QGroupBox* split_canvas_box_ = nullptr;
  QGroupBox* fullscreen_canvas_box_ = nullptr;
  MediaDropTarget* split_left_target_ = nullptr;
  MediaDropTarget* split_right_target_ = nullptr;
  MediaDropTarget* fullscreen_target_ = nullptr;
  QString split_left_media_;
  QString split_right_media_;
  QString fullscreen_media_;
  QPushButton* compose_bg_swatch_ = nullptr;
  QColor compose_underlay_color_{Qt::black};
  CropSettings split_left_crop_;
  CropSettings split_right_crop_;
  CropSettings fullscreen_crop_;

  QSpinBox* brightness_spin_ = nullptr;

  QPushButton* service_start_btn_ = nullptr;
  QPushButton* service_stop_btn_ = nullptr;
  QPushButton* service_restart_btn_ = nullptr;

  /** Non-modal so the main Media/Playback panel stays usable while Settings is open. */
  QPointer<QDialog> settings_dialog_;

  void open_advanced_settings_dialog() {
    if (settings_dialog_) {
      settings_dialog_->raise();
      settings_dialog_->activateWindow();
      return;
    }

    auto* dlg = new QDialog(this);
    settings_dialog_ = dlg;
    dlg->setWindowTitle("Advanced settings — reed-tpse");
    dlg->setModal(false);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(1000, 720);

    auto* v = new QVBoxLayout(dlg);
    advanced_settings_host_->show();
    v->addWidget(advanced_settings_host_, 1);
    auto* close_row = new QHBoxLayout();
    close_row->addStretch();
    auto* close_btn = new QPushButton("Close");
    connect(close_btn, &QPushButton::clicked, dlg, &QDialog::accept);
    close_row->addWidget(close_btn);
    v->addLayout(close_row);

    connect(dlg, &QDialog::finished, this, [this]() {
      if (!advanced_settings_host_ || !central_panel_) return;
      QWidget* p = advanced_settings_host_->parentWidget();
      if (p && p != central_panel_) {
        if (QLayout* lay = p->layout()) {
          lay->removeWidget(advanced_settings_host_);
        }
        advanced_settings_host_->setParent(central_panel_);
        advanced_settings_host_->hide();
      }
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
  }

  void log_line(const QString& line) { log_box_->appendPlainText(line); }

  void append_log_section(const QString& title, const CommandResult& result) {
    log_line("=== " + title + " ===");
    log_line("exit=" + QString::number(result.exit_code));
    const QString out = result.output.trimmed();
    if (out.isEmpty()) {
      log_line("(no entries)");
    } else {
      log_line(out);
    }
    log_line("");
  }

  void append_service_diagnostics_logs() {
    log_line("Collecting service diagnostics logs...");

    const CommandResult svc_status = run_command(
        "systemctl", {"--user", "status", "reed-tpse.service", "--no-pager"}, 20000);
    append_log_section("Service Status", svc_status);

    const CommandResult svc_logs =
        run_command("journalctl",
                    {"--user", "-u", "reed-tpse.service", "--since", "48 hours ago",
                     "--no-pager", "-n", "300"},
                    20000);
    append_log_section("Service Journal (48h)", svc_logs);

    const CommandResult svc_focus = run_command(
        "journalctl",
        {"--user", "-u", "reed-tpse.service", "--since", "48 hours ago", "--no-pager",
         "-n", "300", "--grep",
         "Keepalive handshake failed|Connection appears stale|Attempting reconnect|"
         "Reconnected and restored display state|Handshake failed after reconnect|"
         "Failed to restore display state"},
        20000);
    append_log_section("Service Recovery Events (filtered)", svc_focus);

    const CommandResult kernel_focus = run_command(
        "journalctl",
        {"-k", "--since", "48 hours ago", "--no-pager", "-n", "500", "--grep",
         "ttyACM|cdc_acm|usb .*disconnect|reset .*USB|suspend|resume|PM:|error -71|"
         "error -32|unable to enumerate USB device"},
        20000);
    append_log_section("Kernel USB/Idle Events (filtered)", kernel_focus);

    log_line("Diagnostics log capture complete.");
  }

  void clear_media_cache_clicked() {
    const QString path = QDir::toNativeSeparators(media_cache_dir_);
    const auto ret = QMessageBox::question(
        this, "Clear media cache",
        "Delete everything under the media cache folder?\n\n" + path +
            "\n\nPreview thumbnails (separate folder) are kept unless you delete them "
            "manually.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    QDir dir(media_cache_dir_);
    if (!dir.exists()) {
      QDir().mkpath(media_cache_dir_);
      log_line("[media cache] Nothing to clear (folder recreated).");
      return;
    }
    if (!dir.removeRecursively()) {
      log_line("[media cache] ERROR: could not fully clear " + path);
      QDir().mkpath(media_cache_dir_);
      return;
    }
    QDir().mkpath(media_cache_dir_);
    log_line("[media cache] Cleared and recreated empty folder: " + path);
    refresh_remote_media();
  }

  std::string selected_port() const {
    const QString custom = custom_port_edit_->text().trimmed();
    if (!custom.isEmpty()) return custom.toStdString();
    const QString combo = device_combo_->currentText().trimmed();
    if (!combo.isEmpty()) return combo.toStdString();
    return "";
  }

  std::optional<std::string> resolved_port() {
    std::string port = selected_port();
    if (!port.empty()) return port;
    auto detected = reed::Device::find_device(false);
    return detected;
  }

  QGroupBox* build_health_group(QWidget* parent) {
    auto* group = new QGroupBox("Environment / Health", parent);
    auto* layout = new QHBoxLayout(group);

    group_status_ = new QLabel("Groups: unknown");
    adb_status_ = new QLabel("ADB: unknown");
    serial_status_ = new QLabel("Serial access: unknown");
    service_status_ = new QLabel("Service: unknown");
    storage_status_ = new QLabel("Storage: unknown");

    auto* refresh_btn = new QPushButton("Refresh");
    connect(refresh_btn, &QPushButton::clicked, this, [this]() {
      refresh_health();
      refresh_service_status();
    });

    layout->addWidget(group_status_, 1);
    layout->addWidget(adb_status_, 1);
    layout->addWidget(serial_status_, 1);
    layout->addWidget(service_status_, 1);
    layout->addWidget(storage_status_, 1);
    layout->addWidget(refresh_btn);
    return group;
  }

  QGroupBox* build_device_group(QWidget* parent) {
    auto* group = new QGroupBox("Device Selection", parent);
    auto* layout = new QHBoxLayout(group);

    device_combo_ = new QComboBox();
    device_combo_->setMinimumWidth(220);
    device_combo_->setEditable(false);

    custom_port_edit_ = new QLineEdit();
    custom_port_edit_->setPlaceholderText("/dev/ttyACM0");

    auto* scan_btn = new QPushButton("Scan");
    auto* save_btn = new QPushButton("Save Port");

    connect(scan_btn, &QPushButton::clicked, this, [this]() { refresh_devices(); });
    connect(save_btn, &QPushButton::clicked, this, [this]() { save_config(); });

    layout->addWidget(new QLabel("Detected ports:"));
    layout->addWidget(device_combo_, 1);
    layout->addWidget(new QLabel("Manual override:"));
    layout->addWidget(custom_port_edit_, 1);
    layout->addWidget(scan_btn);
    layout->addWidget(save_btn);
    return group;
  }

  QGroupBox* build_settings_utilities_group(QWidget* parent) {
    auto* group = new QGroupBox("Media cache & device info", parent);
    auto* layout = new QHBoxLayout(group);
    auto* open_cache_btn = new QPushButton("Open media cache folder");
    auto* clear_cache_btn = new QPushButton("Clear media cache…");
    auto* info_btn = new QPushButton("Device Info");
    connect(open_cache_btn, &QPushButton::clicked, this, [this]() {
      QDesktopServices::openUrl(QUrl::fromLocalFile(media_cache_dir_));
    });
    connect(clear_cache_btn, &QPushButton::clicked, this,
            [this]() { clear_media_cache_clicked(); });
    connect(info_btn, &QPushButton::clicked, this, [this]() { show_device_info(); });
    layout->addWidget(open_cache_btn);
    layout->addWidget(clear_cache_btn);
    layout->addStretch(1);
    layout->addWidget(info_btn);
    return group;
  }

  QGroupBox* build_media_group() {
    auto* group = new QGroupBox("Media");
    auto* layout = new QVBoxLayout(group);

    remote_media_list_ = new RemoteMediaListWidget();
    remote_media_list_->setViewMode(QListView::IconMode);
    remote_media_list_->setResizeMode(QListView::Adjust);
    remote_media_list_->setUniformItemSizes(true);
    remote_media_list_->setIconSize(QSize(160, 90));
    remote_media_list_->setGridSize(QSize(190, 140));
    remote_media_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // IconMode collapses badly when the viewport gets tiny; keep ≥ ~2 columns × ~2 rows.
    remote_media_list_->setMinimumSize(400, 300);
    remote_media_list_->setSizePolicy(QSizePolicy::MinimumExpanding,
                                      QSizePolicy::MinimumExpanding);
    remote_media_list_->setDragEnabled(true);
    remote_media_list_->setDragDropMode(QAbstractItemView::DragDrop);
    remote_media_list_->setToolTip("Drag files here to upload to the device");
    remote_media_list_->on_files_dropped = [this](const QStringList& files) {
      for (const QString& f : files) upload_file(f);
      refresh_remote_media();
    };

    auto* refresh_btn = new QPushButton("Refresh List");
    auto* delete_btn = new QPushButton("Delete Selected");
    const QString media_action_btn_style =
        "QPushButton {"
        "  padding: 6px 10px;"
        "  border: 1px solid #5c5c5c;"
        "  border-radius: 6px;"
        "  background-color: #2f2f2f;"
        "}"
        "QPushButton:hover { background-color: #3a3a3a; }"
        "QPushButton:pressed { background-color: #252525; }";
    refresh_btn->setStyleSheet(media_action_btn_style);
    delete_btn->setStyleSheet(media_action_btn_style);
    refresh_btn->setMinimumHeight(30);
    delete_btn->setMinimumHeight(30);

    connect(refresh_btn, &QPushButton::clicked, this,
            [this]() { refresh_remote_media(); });
    connect(delete_btn, &QPushButton::clicked, this, [this]() { delete_selected_media(); });

    auto* header_row = new QHBoxLayout();
    header_row->addWidget(new QLabel("Device Media Files — drag files here to upload"));
    header_row->addStretch(1);
    header_row->addWidget(refresh_btn);
    header_row->addWidget(delete_btn);

    layout->addLayout(header_row);
    layout->addWidget(remote_media_list_);
    return group;
  }

  QGroupBox* build_playback_group() {
    auto* group = new QGroupBox("Playback");
    auto* layout = new QVBoxLayout(group);

    auto* canvas_row = new QHBoxLayout();
    // Not checkable: Qt disables children of an unchecked checkable QGroupBox,
    // which blocks drag-and-drop onto the inactive canvas.
    split_canvas_box_ = new QGroupBox("Split Canvas");
    auto* split_layout = new QHBoxLayout(split_canvas_box_);
    split_left_target_ =
        new MediaDropTarget("Left Slot", CropPreviewWidget::CropShape::k1x1,
                            QSize(220, 220));
    split_right_target_ =
        new MediaDropTarget("Right Slot", CropPreviewWidget::CropShape::k1x1,
                            QSize(220, 220));
    split_layout->addWidget(split_left_target_);
    split_layout->addWidget(split_right_target_);

    fullscreen_canvas_box_ = new QGroupBox("Fullscreen Canvas");
    auto* full_layout = new QVBoxLayout(fullscreen_canvas_box_);
    fullscreen_target_ = new MediaDropTarget(
        "Fullscreen", CropPreviewWidget::CropShape::k2x1, QSize(320, 160));
    full_layout->addWidget(fullscreen_target_);

    canvas_row->addWidget(split_canvas_box_, 1);
    canvas_row->addWidget(fullscreen_canvas_box_, 1);

    brightness_spin_ = new QSpinBox();
    brightness_spin_->setRange(0, 100);

    auto* controls_row = new QHBoxLayout();
    auto* apply_btn = new QPushButton("Apply Display");
    apply_btn->setToolTip("Push display config and brightness to the device");
    auto* settings_btn = new QPushButton("Settings…");
    settings_btn->setToolTip(
        "Environment, USB port, media cache, device info, systemd daemon, log");

    connect(apply_btn, &QPushButton::clicked, this, [this]() { apply_display(); });
    connect(settings_btn, &QPushButton::clicked, this,
            [this]() { open_advanced_settings_dialog(); });
    auto activate_split = [this]() { set_active_canvas_mode(CanvasMode::Split); };
    auto activate_full = [this]() { set_active_canvas_mode(CanvasMode::Fullscreen); };
    split_left_target_->on_drag_enter_from_list = activate_split;
    split_right_target_->on_drag_enter_from_list = activate_split;
    fullscreen_target_->on_drag_enter_from_list = activate_full;

    split_left_target_->on_media_dropped = [this](const QString& media) {
      set_slot_media(split_left_target_, split_left_media_, media);
      set_active_canvas_mode(CanvasMode::Split);
    };
    split_right_target_->on_media_dropped = [this](const QString& media) {
      set_slot_media(split_right_target_, split_right_media_, media);
      set_active_canvas_mode(CanvasMode::Split);
    };
    fullscreen_target_->on_media_dropped = [this](const QString& media) {
      set_slot_media(fullscreen_target_, fullscreen_media_, media);
      set_active_canvas_mode(CanvasMode::Fullscreen);
    };
    split_left_target_->on_clear_requested = [this]() {
      split_left_media_.clear();
      split_left_crop_ = CropSettings{};
      split_left_target_->set_crop_settings(split_left_crop_);
      split_left_target_->set_media("", QIcon());
    };
    split_right_target_->on_clear_requested = [this]() {
      split_right_media_.clear();
      split_right_crop_ = CropSettings{};
      split_right_target_->set_crop_settings(split_right_crop_);
      split_right_target_->set_media("", QIcon());
    };
    fullscreen_target_->on_clear_requested = [this]() {
      fullscreen_media_.clear();
      fullscreen_crop_ = CropSettings{};
      fullscreen_target_->set_crop_settings(fullscreen_crop_);
      fullscreen_target_->set_media("", QIcon());
    };

    compose_bg_swatch_ = new QPushButton();
    compose_bg_swatch_->setFixedSize(44, 28);
    compose_bg_swatch_->setCursor(Qt::PointingHandCursor);
    compose_bg_swatch_->setToolTip(
        "Click to choose underlay color (transparent GIFs / alpha → MP4)");
    connect(compose_bg_swatch_, &QPushButton::clicked, this, [this]() {
      QColor c =
          QColorDialog::getColor(compose_underlay_color_, this, "Underlay color");
      if (c.isValid()) {
        compose_underlay_color_ = c;
        update_compose_bg_swatch();
      }
    });

    controls_row->addWidget(new QLabel("Brightness"));
    controls_row->addWidget(brightness_spin_);
    controls_row->addWidget(new QLabel("Underlay"));
    controls_row->addWidget(compose_bg_swatch_);
    controls_row->addWidget(apply_btn);
    controls_row->addWidget(settings_btn);

    layout->addLayout(canvas_row);
    layout->addLayout(controls_row);
    set_active_canvas_mode(CanvasMode::Fullscreen);
    update_compose_bg_swatch();
    return group;
  }

  void update_compose_bg_swatch() {
    if (!compose_bg_swatch_) return;
    const QString c = compose_underlay_color_.name();
    compose_bg_swatch_->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; border: 2px solid #555; "
                       "border-radius: 3px; min-width: 40px; min-height: 24px; }"
                       "QPushButton:hover { border-color: #888; }")
            .arg(c));
  }

  void set_active_canvas_mode(CanvasMode mode) {
    active_mode_ = mode;
    if (!split_canvas_box_ || !fullscreen_canvas_box_) return;

    const auto active_ss = QStringLiteral(
        "QGroupBox { border: 2px solid #4a9eff; border-radius: 6px; margin-top: 10px; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }");
    const auto idle_ss = QStringLiteral(
        "QGroupBox { border: 1px solid #555; border-radius: 6px; margin-top: 10px; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; "
        "color: #b0b0b0; }");
    split_canvas_box_->setStyleSheet(mode == CanvasMode::Split ? active_ss : idle_ss);
    fullscreen_canvas_box_->setStyleSheet(mode == CanvasMode::Fullscreen ? active_ss
                                                                         : idle_ss);
  }

  QGroupBox* build_service_group(QWidget* parent) {
    auto* group = new QGroupBox("Daemon / Service", parent);
    auto* layout = new QHBoxLayout(group);

    service_start_btn_ = new QPushButton("Start");
    service_stop_btn_ = new QPushButton("Stop");
    service_restart_btn_ = new QPushButton("Restart");
    auto* enable_btn = new QPushButton("Enable");
    auto* status_btn = new QPushButton("Status");
    auto* logs_btn = new QPushButton("View Logs");

    connect(service_start_btn_, &QPushButton::clicked, this,
            [this]() { run_service_cmd({"start", "reed-tpse.service"}); });
    connect(service_stop_btn_, &QPushButton::clicked, this,
            [this]() { run_service_cmd({"stop", "reed-tpse.service"}); });
    connect(service_restart_btn_, &QPushButton::clicked, this,
            [this]() { run_service_cmd({"restart", "reed-tpse.service"}); });
    connect(enable_btn, &QPushButton::clicked, this,
            [this]() { run_service_cmd({"enable", "reed-tpse.service"}); });
    connect(status_btn, &QPushButton::clicked, this, [this]() {
      run_service_cmd({"status", "reed-tpse.service"});
      refresh_service_status();
    });
    connect(logs_btn, &QPushButton::clicked, this, [this]() {
      append_service_diagnostics_logs();
    });

    layout->addWidget(service_start_btn_);
    layout->addWidget(service_stop_btn_);
    layout->addWidget(service_restart_btn_);
    layout->addWidget(enable_btn);
    layout->addWidget(status_btn);
    layout->addWidget(logs_btn);
    layout->addStretch();
    return group;
  }

  void refresh_devices() {
    device_combo_->clear();
    std::vector<std::string> found;

    try {
      for (const auto& entry : fs::directory_iterator("/dev")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("ttyACM", 0) == 0) {
          found.push_back(entry.path().string());
        }
      }
    } catch (...) {
      log_line("Failed to scan /dev for ttyACM devices.");
    }

    std::sort(found.begin(), found.end());
    for (const auto& port : found) {
      device_combo_->addItem(QString::fromStdString(port));
    }

    if (found.empty()) {
      device_combo_->addItem("<none>");
    }
  }

  void refresh_health() {
    CommandResult groups = run_command("id", {"-nG"});
    const QString group_text = groups.output.trimmed();
    const bool has_serial_group =
        group_text.contains("uucp") || group_text.contains("dialout");
    group_status_->setText(QString("Groups: ") +
                           (has_serial_group ? "OK" : "Missing uucp/dialout"));

    const bool adb_ok = reed::Adb::is_device_connected();
    adb_status_->setText(QString("ADB: ") + (adb_ok ? "Connected" : "Disconnected"));

    bool serial_ok = false;
    const std::string port = selected_port();
    if (!port.empty()) {
      serial_ok = access(port.c_str(), R_OK | W_OK) == 0;
    }
    serial_status_->setText(QString("Serial access: ") + (serial_ok ? "OK" : "No RW access"));

    auto storage = reed::Adb::get_media_storage_info();
    if (!storage) {
      storage_status_->setText("Storage: unknown");
    } else {
      const QString used = human_size_kb(storage->used_kb);
      const QString total = human_size_kb(storage->total_kb);
      const QString free = human_size_kb(storage->available_kb);
      storage_status_->setText("Storage: " + used + " / " + total + " (free " +
                               free + ")");
    }
  }

  void refresh_service_status() {
    CommandResult result =
        run_command("systemctl", {"--user", "is-active", "reed-tpse.service"});
    const QString state = result.output.trimmed();
    service_status_->setText("Service: " + (state.isEmpty() ? "unknown" : state));
  }

  void refresh_remote_media() {
    remote_media_list_->clear();
    auto media = reed::Adb::list_media();
    if (!media) {
      log_line("Failed to list media via ADB.");
      return;
    }
    cleanup_generated_split_media(*media);
    if (auto all_media = reed::Adb::list_media_all()) {
      cleanup_generated_bgmix_media(*all_media);
    }

    for (const auto& file : *media) {
      if (is_generated_split_media(file)) {
        // Keep generated split artifacts out of user media picker.
        continue;
      }
      if (is_generated_bgmix_media(file)) {
        continue;
      }
      const QString file_q = QString::fromStdString(file);
      const QString local_file = sync_media_to_cache(file_q);
      const QIcon icon = build_thumbnail_icon(file_q, local_file);
      auto* item = new QListWidgetItem(icon, file_q);
      item->setData(Qt::UserRole, file_q);
      remote_media_list_->addItem(item);
    }
    refresh_slot_previews();
    log_line("Loaded media previews (" + QString::number(remote_media_list_->count()) +
             " items).");
    refresh_health();
  }

  void cleanup_generated_split_media(const std::vector<std::string>& media_files) {
    std::string active_split;
    if (auto state = reed::ConfigManager::load_state()) {
      if (!state->media.empty() && is_generated_split_media(state->media.front())) {
        active_split = state->media.front();
      }
    }

    int removed = 0;
    for (const auto& file : media_files) {
      if (!is_generated_split_media(file)) continue;
      if (!active_split.empty() && file == active_split) continue;
      if (reed::Adb::remove(file)) {
        ++removed;
      }
    }
    if (removed > 0) {
      log_line("Cleaned up stale generated split files: " + QString::number(removed));
    }
  }

  void cleanup_generated_bgmix_media(const std::vector<std::string>& media_files) {
    std::string active_bgmix;
    if (auto state = reed::ConfigManager::load_state()) {
      if (!state->media.empty() &&
          is_generated_bgmix_media(state->media.front())) {
        active_bgmix = state->media.front();
      }
    }

    int removed = 0;
    for (const auto& file : media_files) {
      if (!is_generated_bgmix_media(file)) continue;
      if (!active_bgmix.empty() && file == active_bgmix) continue;
      if (delete_media_on_device(file)) {
        ++removed;
      }
    }
    if (removed > 0) {
      log_line("Removed stale composed fullscreen (bgmix) files: " +
               QString::number(removed));
    }
  }

  void purge_stale_composed_remote_files() {
    if (auto media = reed::Adb::list_media()) {
      cleanup_generated_split_media(*media);
    }
    if (auto all_media = reed::Adb::list_media_all()) {
      cleanup_generated_bgmix_media(*all_media);
    }
  }

  /** One file per remote name under media_cache_dir_: pull once, reuse until cleared. */
  QString sync_media_to_cache(const QString& remote_name) {
    const QString local_path = media_cache_dir_ + "/" + remote_name;
    if (!QFileInfo::exists(local_path)) {
      log_line("Cache miss, pulling: " + remote_name);
      if (!reed::Adb::pull(remote_name.toStdString(), local_path.toStdString())) {
        log_line("adb pull failed: " + remote_name);
      }
    }
    return local_path;
  }

  QIcon build_thumbnail_icon(const QString& remote_name, const QString& local_path) {
    const QString thumb_path = thumb_cache_dir_ + "/" + remote_name + ".png";
    if (!QFileInfo::exists(thumb_path)) {
      const std::string local = local_path.toStdString();
      const reed::MediaType type = reed::Media::detect_type(local);
      bool thumb_ok = false;
      if (type == reed::MediaType::Image) {
        QImageReader reader(local_path);
        QImage image = reader.read();
        if (!image.isNull()) {
          image.scaled(160, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)
              .save(thumb_path, "PNG");
          thumb_ok = true;
        }
      } else {
        CommandResult result = run_command(
            "ffmpeg",
            {"-y", "-i", local_path, "-vf", "thumbnail,scale=320:-1", "-frames:v",
             "1", thumb_path},
            30000);
        thumb_ok = result.exit_code == 0;
      }
      if (!thumb_ok) {
        QImage placeholder(160, 90, QImage::Format_ARGB32);
        placeholder.fill(QColor(50, 50, 50));
        placeholder.save(thumb_path, "PNG");
      }
    }
    return QIcon(thumb_path);
  }

  QListWidgetItem* find_media_item_by_name(const QString& media) {
    for (int i = 0; i < remote_media_list_->count(); ++i) {
      QListWidgetItem* item = remote_media_list_->item(i);
      const QString item_name = item->data(Qt::UserRole).toString();
      if (item_name == media || item->text() == media) return item;
    }
    return nullptr;
  }

  void set_slot_media(MediaDropTarget* target, QString& slot, const QString& media) {
    const QString previous = slot;
    slot = media.trimmed();
    QListWidgetItem* item = find_media_item_by_name(slot);
    target->set_media(slot, item ? item->icon() : QIcon());
    if (target == split_left_target_) {
      if (previous != slot) split_left_crop_ = CropSettings{};
      split_left_target_->set_crop_settings(split_left_crop_);
    } else if (target == split_right_target_) {
      if (previous != slot) split_right_crop_ = CropSettings{};
      split_right_target_->set_crop_settings(split_right_crop_);
    } else if (target == fullscreen_target_) {
      if (previous != slot) fullscreen_crop_ = CropSettings{};
      fullscreen_target_->set_crop_settings(fullscreen_crop_);
    }
    log_line("Assigned media: " + slot);
  }

  void refresh_slot_previews() {
    if (!split_left_media_.isEmpty()) {
      set_slot_media(split_left_target_, split_left_media_, split_left_media_);
    }
    if (!split_right_media_.isEmpty()) {
      set_slot_media(split_right_target_, split_right_media_, split_right_media_);
    }
    if (!fullscreen_media_.isEmpty()) {
      set_slot_media(fullscreen_target_, fullscreen_media_, fullscreen_media_);
    }
  }

  std::optional<SplitComposeResult> compose_split_media(
      const QString& left_media, const QString& right_media,
      const CropSettings& left_crop, const CropSettings& right_crop,
      const QColor& underlay_color) {
    const QString left_local = sync_media_to_cache(left_media);
    const QString right_local = sync_media_to_cache(right_media);
    if (!QFileInfo::exists(left_local) || !QFileInfo::exists(right_local)) {
      log_line("Split compose failed: missing local cache source(s).");
      return std::nullopt;
    }

    const bool has_gif =
        reed::Media::detect_type(left_local.toStdString()) ==
            reed::MediaType::Gif ||
        reed::Media::detect_type(right_local.toStdString()) ==
            reed::MediaType::Gif;

    const std::string compose_version = "split_underlay_v4_cover_crop";
    const std::string key =
        compose_version + "|" + left_media.toStdString() + "|" +
        right_media.toStdString() + "|" +
        underlay_color.name(QColor::HexRgb).toStdString();
    const size_t hash = std::hash<std::string>{}(key);
    const QString hash_q = QString::number(static_cast<qulonglong>(hash), 16);
    const QString out_local = media_cache_dir_ + "/tmp_split_" + hash_q + ".mp4";
    const QString out_remote = "split_" + hash_q + ".mp4";

    const QString lavfi_base =
        QString("color=c=0x%1%2%3:s=%4x%5:r=%6")
            .arg(underlay_color.red(), 2, 16, QChar('0'))
            .arg(underlay_color.green(), 2, 16, QChar('0'))
            .arg(underlay_color.blue(), 2, 16, QChar('0'))
            .arg(reed::panel::kWidth)
            .arg(reed::panel::kHeight)
            .arg(reed::panel::kFps);

    const auto left_duration = probe_duration_seconds(left_local);
    const auto right_duration = probe_duration_seconds(right_local);
    const double raw_target =
        std::max(left_duration.value_or(12.0), right_duration.value_or(12.0));
    const double target_duration = clamp_compose_duration(
        raw_target, "Split compose",
        [this](const QString& s) { log_line(s); });
    const QString target_duration_q =
        QString::number(std::ceil(target_duration * 1000.0) / 1000.0, 'f', 3);

    auto probe_dimensions = [](const QString& path) -> std::optional<QSize> {
      CommandResult result = run_command(
          "ffprobe",
          {"-v", "error", "-select_streams", "v:0", "-show_entries",
           "stream=width,height", "-of", "csv=s=x:p=0", path});
      if (result.exit_code != 0) return std::nullopt;
      const QString out = result.output.trimmed();
      const QStringList parts = out.split("x");
      if (parts.size() != 2) return std::nullopt;
      bool ok_w = false;
      bool ok_h = false;
      const int w = parts[0].toInt(&ok_w);
      const int h = parts[1].toInt(&ok_h);
      if (!ok_w || !ok_h || w <= 0 || h <= 0) return std::nullopt;
      return QSize(w, h);
    };
    auto crop_filter = [](const QSize& src, const CropSettings& crop,
                          const QString& input, const QString& output) -> QString {
      const int min_dim = std::min(src.width(), src.height());
      const int crop_size =
          std::max(32, static_cast<int>(std::round(min_dim * crop.scale)));
      const int max_x = std::max(0, src.width() - crop_size);
      const int max_y = std::max(0, src.height() - crop_size);
      const int x = std::clamp(static_cast<int>(std::round(max_x * crop.cx)), 0, max_x);
      const int y = std::clamp(static_cast<int>(std::round(max_y * crop.cy)), 0, max_y);
      // Scale-to-cover then center-crop so each half is filled (no side bars). "decrease"
      // + pad left transparent/underlay gaps when crop aspect ≠ 1120×1080.
      return QString(
                 "[%1:v]fps=%6,format=rgba,crop=%2:%2:%3:%4,scale=%7:%8:force_original_"
                 "aspect_ratio=increase:flags=neighbor,crop=%7:%8:(iw-%7)/2:(ih-%8)/2,"
                 "setsar=1[%5];")
          .arg(input)
          .arg(crop_size)
          .arg(x)
          .arg(y)
          .arg(output)
          .arg(reed::panel::kFps)
          .arg(reed::panel::kSplitHalfWidth)
          .arg(reed::panel::kSplitHalfHeight);
    };
    const QSize left_size =
        probe_dimensions(left_local).value_or(QSize(1080, 1080));
    const QSize right_size =
        probe_dimensions(right_local).value_or(QSize(1080, 1080));
    const QString crop_graph = crop_filter(left_size, left_crop, "0", "l") +
                               crop_filter(right_size, right_crop, "1", "r");
    // GIF path: stack on solid underlay (picker color), one H.264 MP4 (no WebM).
    const QString dar_out =
        QString("format=yuv420p,setsar=%1,setdar=%2")
            .arg(reed::panel::kFfmpegSar)
            .arg(reed::panel::kFfmpegDar);
    const QString filter =
        has_gif ? (crop_graph +
                   "[l][r]hstack=inputs=2,format=rgba[hs];"
                   "[2:v]format=rgba[base];"
                   "[base][hs]overlay=format=auto:alpha=straight[flat];"
                   "[flat]" + dar_out + "[v];")
                : (crop_graph +
                   "[l][r]hstack=inputs=2,format=rgba[hs];[hs]" + dar_out + "[v];");

    QStringList compose_args;
    compose_args << "-y";
    auto add_looping_video_input = [&](const QString& path) {
      if (reed::Media::detect_type(path.toStdString()) == reed::MediaType::Gif) {
        compose_args << "-ignore_loop" << "1";
      }
      compose_args << "-stream_loop" << "-1" << "-i" << path;
    };
    add_looping_video_input(left_local);
    add_looping_video_input(right_local);
    if (has_gif) {
      compose_args << "-f"
                   << "lavfi"
                   << "-i" << lavfi_base;
    }
    compose_args << "-filter_complex" << filter << "-t" << target_duration_q << "-map"
                 << "[v]"
                 << "-an"
                 << "-c:v"
                 << "libx264"
                 << "-preset"
                 << "veryfast"
                 << "-profile:v"
                 << "high"
                 << "-level"
                 << "5.1"
                 << "-pix_fmt"
                 << "yuv420p"
                 << "-r"
                 << QString::number(reed::panel::kFps)
                 << "-vsync"
                 << "cfr"
                 << "-g"
                 << "120"
                 << "-keyint_min"
                 << "120"
                 << "-sc_threshold"
                 << "0"
                 << "-b:v"
                 << "12M"
                 << "-maxrate"
                 << "15M"
                 << "-bufsize"
                 << "24M"
                 << "-movflags"
                 << "+faststart" << out_local;

    CommandResult compose = run_command("ffmpeg", compose_args, 300000);
    if (compose.exit_code != 0) {
      log_line("Split compose failed: ffmpeg error");
      log_line(compose.output.trimmed());
      return std::nullopt;
    }
    log_line("Composed split: " + target_duration_q + "s" +
             (has_gif ? " (GIF on underlay " + underlay_color.name(QColor::HexRgb) + ")"
                      : ""));

    if (!reed::Adb::push(out_local.toStdString(), out_remote.toStdString())) {
      log_line("Failed to upload composed split media: " + out_remote);
      return std::nullopt;
    }
    std::error_code ec;
    fs::remove(out_local.toStdString(), ec);
    SplitComposeResult result;
    result.remote_mp4 = out_remote.toStdString();
    log_line("Composed and uploaded split media: " + out_remote);
    return result;
  }

  std::optional<std::string> compose_foreground_with_background(
      const std::string& foreground_remote, const QColor& underlay_color,
      const QString& foreground_local_override = {},
      const CropSettings& fg_crop = {}, bool fg_crop_shape_2x1 = true) {
    const QString fg_local =
        !foreground_local_override.isEmpty() &&
                QFileInfo::exists(foreground_local_override)
            ? foreground_local_override
            : sync_media_to_cache(QString::fromStdString(foreground_remote));
    if (!QFileInfo::exists(fg_local)) {
      log_line("Background compose failed: foreground missing in cache.");
      return std::nullopt;
    }

    const bool fg_is_gif =
        reed::Media::detect_type(fg_local.toStdString()) == reed::MediaType::Gif;
    log_line("Underlay compose input: " + fg_local + (fg_is_gif ? " (GIF)" : ""));

    // Native panel size (2240×1080): solid base + centered foreground.
    const int kCanvasW = reed::panel::kWidth;
    const int kCanvasH = reed::panel::kHeight;

    const QString hash_input =
        QString::fromStdString(foreground_remote) + "|" +
        underlay_color.name(QColor::HexRgb) + "|bg_simple_v5_cover_crop_dar21_sar2728|" +
        QString::number(fg_crop.cx, 'g', 9) + "," +
        QString::number(fg_crop.cy, 'g', 9) + "," +
        QString::number(fg_crop.scale, 'g', 9) + "|" +
        (fg_crop_shape_2x1 ? "2x1" : "1x1");
    const size_t hash = std::hash<std::string>{}(hash_input.toStdString());
    const QString hash_q = QString::number(static_cast<qulonglong>(hash), 16);
    const QString out_local = media_cache_dir_ + "/tmp_bg_" + hash_q + ".mp4";
    // Leading '.' hides file from `ls -1` (device UI + our media grid); see cleanup.
    const QString out_remote = ".bgmix_" + hash_q + ".mp4";

    std::optional<double> fg_duration = probe_duration_seconds(fg_local);
    double target_duration = fg_duration.value_or(12.0);

    QStringList args;
    args << "-y";
    // Input 0: infinite solid-color video at target resolution (picker color).
    const QString lavfi_underlay =
        QString("color=c=0x%1%2%3:s=%4x%5:r=%6")
            .arg(underlay_color.red(), 2, 16, QChar('0'))
            .arg(underlay_color.green(), 2, 16, QChar('0'))
            .arg(underlay_color.blue(), 2, 16, QChar('0'))
            .arg(kCanvasW)
            .arg(kCanvasH)
            .arg(reed::panel::kFps);
    args << "-f" << "lavfi" << "-i" << lavfi_underlay;

    if (fg_is_gif) {
      args << "-ignore_loop" << "1";
    }
    args << "-stream_loop" << "-1" << "-i" << fg_local;

    target_duration = clamp_compose_duration(
        target_duration, "Background mix",
        [this](const QString& s) { log_line(s); });

    QSize src_dims(1920, 1080);
    {
      CommandResult probe = run_command(
          "ffprobe",
          {"-v", "error", "-select_streams", "v:0", "-show_entries",
           "stream=width,height", "-of", "csv=s=x:p=0", fg_local},
          30000);
      if (probe.exit_code == 0) {
        const QStringList parts = probe.output.trimmed().split('x');
        if (parts.size() == 2) {
          bool ok_w = false;
          bool ok_h = false;
          const int w = parts[0].toInt(&ok_w);
          const int h = parts[1].toInt(&ok_h);
          if (ok_w && ok_h && w > 0 && h > 0) src_dims = QSize(w, h);
        }
      }
    }

    // Cover full panel: cropped 2:1 (or other) region is rarely exactly 2240×1080 (56:27).
    const QString scale_fg =
        QString("scale=%1:%2:force_original_aspect_ratio=increase:flags=neighbor,"
                "crop=%1:%2:(iw-%1)/2:(ih-%2)/2")
            .arg(kCanvasW)
            .arg(kCanvasH);

    QString fg_preprocess = scale_fg;
    if (auto cr = ffmpeg_crop_rect_pixels(src_dims, fg_crop, fg_crop_shape_2x1)) {
      fg_preprocess =
          QString("crop=%1:%2:%3:%4,%5")
              .arg(cr->width())
              .arg(cr->height())
              .arg(cr->x())
              .arg(cr->y())
              .arg(scale_fg);
    }

    // Solid color + foreground scaled/centered; GIF uses native decoder alpha in graph.
    const QString filter =
        QString("[0:v]fps=%2,format=rgba[bg];"
                "[1:v]fps=%2,format=rgba,%1[fg];"
                "[bg][fg]overlay=x=(W-w)/2:y=(H-h)/2:format=auto:alpha=straight[v];"
                "[v]format=yuv420p,setsar=%3,setdar=%4[outv]")
            .arg(fg_preprocess)
            .arg(reed::panel::kFps)
            .arg(reed::panel::kFfmpegSar)
            .arg(reed::panel::kFfmpegDar);

    args << "-filter_complex" << filter << "-map"
         << "[outv]" << "-an" << "-c:v" << "libx264" << "-preset" << "veryfast"
         << "-profile:v" << "high" << "-level" << "5.1" << "-pix_fmt"
         << "yuv420p"
         << "-r"
         << QString::number(reed::panel::kFps)
         << "-vsync"
         << "cfr"
         << "-g"
         << "120"
         << "-keyint_min"
         << "120"
         << "-sc_threshold"
         << "0"
         << "-b:v"
         << "12M"
         << "-maxrate"
         << "15M"
         << "-bufsize"
         << "24M"
         << "-movflags" << "+faststart"
         << "-t" << QString::number(target_duration, 'f', 3) << out_local;

    CommandResult compose = run_command("ffmpeg", args, 180000);
    if (compose.exit_code != 0) {
      log_line("Background compose failed: ffmpeg error");
      log_line(compose.output.trimmed());
      return std::nullopt;
    }

    if (!reed::Adb::push(out_local.toStdString(), out_remote.toStdString())) {
      log_line("Failed to upload composed background media: " + out_remote);
      return std::nullopt;
    }
    std::error_code ec;
    fs::remove(out_local.toStdString(), ec);
    log_line("Composed with background: " + out_remote);
    return out_remote.toStdString();
  }

  void upload_file(const QString& file_path_q) {
    const std::string file_path = file_path_q.toStdString();
    if (!fs::exists(file_path)) {
      log_line("File missing: " + file_path_q);
      return;
    }
    const reed::MediaType type = reed::Media::detect_type(file_path);
    if (type == reed::MediaType::Unknown) {
      log_line("Skipping unsupported media type: " + file_path_q);
      return;
    }
    if (!reed::Adb::is_device_connected()) {
      log_line("No ADB device connected. Cannot upload.");
      return;
    }

    std::string upload_path = file_path;
    std::string remote_name = reed::Media::get_filename(file_path);

    if (type == reed::MediaType::Gif) {
      log_line("Uploading GIF source as-is (conversion happens at Apply).");
    }

    log_line("Uploading: " + QString::fromStdString(remote_name));
    if (!reed::Adb::push(upload_path, remote_name)) {
      log_line("Upload failed: " + QString::fromStdString(remote_name));
      return;
    }
    log_line("Upload complete: " + QString::fromStdString(remote_name));
  }

  /** Prefer USB mediaDelete (matches device app); fall back to adb shell rm -f. */
  bool delete_media_on_device(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) {
      log_line("Delete skipped: invalid name (expected basename only).");
      return false;
    }

    if (auto port = resolved_port()) {
      reed::Device dev(*port, false);
      if (dev.connect() && dev.handshake()) {
        auto resp = dev.delete_media({name});
        if (resp.has_value()) {
          return true;
        }
        log_line("mediaDelete (USB) had no response; trying ADB rm…");
      }
    }

    if (!reed::Adb::is_device_connected()) {
      log_line("No ADB device for fallback delete.");
      return false;
    }
    return reed::Adb::remove(name);
  }

  void delete_selected_media() {
    const QList<QListWidgetItem*> selected = remote_media_list_->selectedItems();
    if (selected.isEmpty()) {
      log_line("No media selected to delete.");
      return;
    }

    for (QListWidgetItem* item : selected) {
      QString name_q = item->data(Qt::UserRole).toString().trimmed();
      if (name_q.isEmpty()) {
        name_q = item->text().trimmed();
      }
      name_q.remove(QChar('\r'));
      name_q.remove(QChar('\n'));

      if (delete_media_on_device(name_q.toStdString())) {
        log_line("Deleted: " + name_q);
      } else {
        log_line("Delete failed: " + name_q +
                 " (open Settings and check USB port + ADB; file may be in use on "
                 "device)");
      }
    }
    refresh_remote_media();
  }

  void show_device_info() {
    auto port = resolved_port();
    if (!port) {
      log_line("No device found. Select or input a port.");
      return;
    }
    reed::Device device(*port, true);
    if (!device.connect()) {
      log_line("Failed to connect to " + QString::fromStdString(*port));
      return;
    }
    auto info = device.handshake();
    if (!info) {
      log_line("Handshake failed on " + QString::fromStdString(*port));
      return;
    }
    QString lines =
        "product=" + QString::fromStdString(info->product_id) + "\n" +
        "serial=" + QString::fromStdString(info->serial) + "\n" +
        "OS=" + QString::fromStdString(info->os) + "\n" +
        "app=" + QString::fromStdString(info->app_version) + "\n" +
        "firmware=" + QString::fromStdString(info->firmware) + "\n" +
        "hardware=" + QString::fromStdString(info->hardware);
    if (!info->attributes.empty()) {
      lines += "\n\nattributes (from device):";
      for (const auto& a : info->attributes) {
        lines += "\n  - " + QString::fromStdString(a);
      }
    } else {
      lines += "\n\n(no attribute[] in handshake — no resolution hints from host)";
    }
    lines +=
        "\n\nNote: Device JSON uses ratio \"" + QString(reed::panel::kDeviceJsonRatio) +
        "\". Video is 2240×1080 @ 60Hz with stream tags 2:1 DAR + matching SAR so "
        "the player should not letterbox for ratio mismatch.";
    log_line("Device info:\n" + lines);
    QMessageBox::information(this, "Device Info", lines);
  }

  void apply_display() {
    auto port = resolved_port();
    if (!port) {
      QMessageBox::warning(this, "No Device",
                           "No device found. Select a port or connect device.");
      return;
    }

    std::vector<std::string> files;
    std::string ratio = reed::panel::kDeviceJsonRatio;
    if (active_mode_ == CanvasMode::Split) {
      if (split_left_media_.isEmpty() || split_right_media_.isEmpty()) {
        QMessageBox::warning(this, "Split Media Missing",
                             "Drop one media in Left and one in Right slot.");
        return;
      }
      split_left_crop_ = split_left_target_->crop_settings();
      split_right_crop_ = split_right_target_->crop_settings();
      auto composed =
          compose_split_media(split_left_media_, split_right_media_, split_left_crop_,
                              split_right_crop_, compose_underlay_color_);
      if (!composed) return;
      files.push_back(composed->remote_mp4);
    } else {
      if (fullscreen_media_.isEmpty()) {
        QMessageBox::warning(this, "Fullscreen Media Missing",
                             "Drop one media into the Fullscreen slot.");
        return;
      }
      files.push_back(fullscreen_media_.toStdString());
    }

    // Fullscreen: composite GIF/image onto underlay. Split: underlay baked in split MP4.
    if (!files.empty()) {
      bool need_layer = false;
      if (active_mode_ == CanvasMode::Fullscreen) {
        const reed::MediaType fg_type = reed::Media::detect_type(files.front());
        need_layer = (fg_type != reed::MediaType::Video);
      }
      if (need_layer) {
        auto layered = compose_foreground_with_background(
            files.front(), compose_underlay_color_, QString(),
            fullscreen_target_->crop_settings(), true);
        if (layered) {
          files.clear();
          files.push_back(*layered);
        } else {
          QMessageBox::warning(
              this, "Underlay compose failed",
              "ffmpeg could not composite onto the underlay color (see log). "
              "The original file will be sent to the device, which often shows a "
              "white or wrong background for GIFs.");
        }
      }
    }

    reed::Device device(*port, false);
    if (!device.connect()) {
      log_line("Failed to connect to " + QString::fromStdString(*port));
      return;
    }

    if (!device.handshake()) {
      log_line("Handshake failed.");
      return;
    }

    reed::ScreenConfig cfg;
    cfg.media = files;
    cfg.ratio = ratio;
    // Keep GUI behavior aligned with known working device defaults.
    cfg.play_mode = "Single";
    cfg.screen_mode = "Full Screen";

    if (!device.set_screen_config(cfg)) {
      log_line("Failed to set screen config.");
      return;
    }
    if (!device.set_brightness(brightness_spin_->value())) {
      log_line("Failed to set brightness.");
      return;
    }

    reed::DisplayState state;
    state.media = files;
    state.ratio = cfg.ratio;
    state.play_mode = cfg.play_mode;
    state.screen_mode = cfg.screen_mode;
    state.brightness = brightness_spin_->value();
    state.compose_bg_color =
        compose_underlay_color_.name(QColor::HexRgb).toStdString();
    reed::ConfigManager::save_state(state);

    log_line("Display applied: " + join_strings(cfg.media) +
             (active_mode_ == CanvasMode::Split ? " [split composed]" : " [fullscreen]") +
             ", brightness " + QString::number(brightness_spin_->value()));
    save_config();
    purge_stale_composed_remote_files();
  }

  void run_service_cmd(const QStringList& args) {
    CommandResult result = run_command("systemctl", QStringList({"--user"}) + args);
    log_line("systemctl " + args.join(" ") + " (exit=" +
             QString::number(result.exit_code) + ")");
    if (!result.output.trimmed().isEmpty()) log_line(result.output.trimmed());
    refresh_service_status();
  }

  void load_config() {
    if (auto cfg = reed::ConfigManager::load_config()) {
      custom_port_edit_->setText(QString::fromStdString(cfg->port));
      brightness_spin_->setValue(cfg->brightness);
    } else {
      brightness_spin_->setValue(75);
    }

    if (auto state = reed::ConfigManager::load_state()) {
      if (!state->media.empty()) {
        fullscreen_media_ = QString::fromStdString(state->media.front());
      }
      brightness_spin_->setValue(state->brightness);
      QColor loaded(QString::fromStdString(state->compose_bg_color));
      if (loaded.isValid()) {
        compose_underlay_color_ = loaded;
        update_compose_bg_swatch();
      }
    }
  }

  void save_config() {
    reed::Config cfg;
    cfg.port = selected_port();
    cfg.brightness = brightness_spin_->value();
    cfg.keepalive_interval = 10;
    if (auto old_cfg = reed::ConfigManager::load_config()) {
      cfg.keepalive_interval = old_cfg->keepalive_interval;
      if (!cfg.port.empty()) old_cfg->port = cfg.port;
      old_cfg->brightness = cfg.brightness;
      if (reed::ConfigManager::save_config(*old_cfg)) {
        log_line("Saved config: " + QString::fromStdString(old_cfg->port));
      } else {
        log_line("Failed to save config.");
      }
      refresh_health();
      return;
    }
    if (reed::ConfigManager::save_config(cfg)) {
      log_line("Saved config: " + QString::fromStdString(cfg.port));
    } else {
      log_line("Failed to save config.");
    }
    refresh_health();
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  MainWindow window;
  window.show();
  return app.exec();
}
