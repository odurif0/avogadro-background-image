/******************************************************************************
  Background Image Scene Plugin for Avogadro 2

  Rendering technique:
    Overlay2DPass (renders last, after all molecule geometry).
    1. Copy the current framebuffer (molecule) to a texture via
       glCopyTexSubImage2D (with glReadPixels fallback for MSAA FBOs)
    2. Draw a single fullscreen quad with a combined GLSL shader that:
       - Samples the background image (with aspect-ratio letterbox/pillarbox)
       - Samples the saved framebuffer
       - Uses luminance of the framebuffer to blend molecule over background
    3. Uses VBO+VAO for OpenGL core profile compatibility
******************************************************************************/

#include "backgroundimageengine.h"

#include <avogadro/rendering/camera.h>

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include <QSizePolicy>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(LOG_BG, "avogadro.plugin.backgroundimage")

namespace Avogadro {

// ==================== BackgroundImageDrawable ====================

// Single combined shader: reads background image + framebuffer, blends by luminance
static const char *combinedVS =
  "attribute vec2 aPos;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
  "  vUV = aPos * 0.5 + 0.5;\n"
  "}\n";

static const char *combinedFS =
  "uniform sampler2D uBackground;\n"
  "uniform sampler2D uFramebuffer;\n"
  "uniform vec2 uBgUVOffset;\n"
  "uniform vec2 uBgUVScale;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  vec2 bgUV = (vUV - uBgUVOffset) / uBgUVScale;\n"
  "  vec4 bg;\n"
  "  if (bgUV.x < 0.0 || bgUV.x > 1.0 || bgUV.y < 0.0 || bgUV.y > 1.0)\n"
  "    bg = vec4(0.0, 0.0, 0.0, 1.0);\n"
  "  else\n"
  "    bg = texture2D(uBackground, bgUV);\n"
  "  vec4 fb = texture2D(uFramebuffer, vUV);\n"
  "  float lum = dot(fb.rgb, vec3(0.299, 0.587, 0.114));\n"
  "  float alpha = smoothstep(0.03, 0.12, lum);\n"
  "  gl_FragColor = mix(bg, fb, alpha);\n"
  "}\n";

BackgroundImageDrawable::BackgroundImageDrawable()
  : m_textureId(0), m_textureDirty(true),
    m_fbCopyTexture(0), m_fbCopyW(0), m_fbCopyH(0),
    m_shaderProgram(0), m_shaderReady(false),
    m_keepAspectRatio(true)
{
  setRenderPass(Rendering::Overlay2DPass);
}

BackgroundImageDrawable::~BackgroundImageDrawable()
{
  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if (!ctx) return;
  QOpenGLFunctions *gl = ctx->functions();
  if (m_textureId) { gl->glDeleteTextures(1, &m_textureId); m_textureId = 0; }
  if (m_fbCopyTexture) { gl->glDeleteTextures(1, &m_fbCopyTexture); m_fbCopyTexture = 0; }
  if (m_shaderProgram) { gl->glDeleteProgram(m_shaderProgram); m_shaderProgram = 0; }
}

void BackgroundImageDrawable::setImage(const QImage &image)
{
  m_image = image;
  m_textureDirty = true;
}

void BackgroundImageDrawable::setKeepAspectRatio(bool keep)
{
  m_keepAspectRatio = keep;
}

void BackgroundImageDrawable::clearImage()
{
  m_image = QImage();
  m_textureDirty = false;
  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if (ctx && m_textureId) {
    ctx->functions()->glDeleteTextures(1, &m_textureId);
    m_textureId = 0;
  }
}

static GLuint compileShader(QOpenGLFunctions *gl, GLenum type, const char *src)
{
  GLuint s = gl->glCreateShader(type);
  gl->glShaderSource(s, 1, &src, nullptr);
  gl->glCompileShader(s);
  GLint ok = 0;
  gl->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    gl->glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    qCWarning(LOG_BG, "Shader compile error: %s", log);
    gl->glDeleteShader(s);
    return 0;
  }
  return s;
}

bool BackgroundImageDrawable::ensureShader()
{
  if (m_shaderReady) return m_shaderProgram != 0;

  m_shaderReady = true;
  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if (!ctx) return false;
  QOpenGLFunctions *gl = ctx->functions();

  GLuint vs = compileShader(gl, GL_VERTEX_SHADER, combinedVS);
  GLuint fs = compileShader(gl, GL_FRAGMENT_SHADER, combinedFS);
  if (!vs || !fs) {
    if (vs) gl->glDeleteShader(vs);
    if (fs) gl->glDeleteShader(fs);
    return false;
  }

  m_shaderProgram = gl->glCreateProgram();
  gl->glAttachShader(m_shaderProgram, vs);
  gl->glAttachShader(m_shaderProgram, fs);
  gl->glBindAttribLocation(m_shaderProgram, 0, "aPos");
  gl->glLinkProgram(m_shaderProgram);

  GLint ok = 0;
  gl->glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    gl->glGetProgramInfoLog(m_shaderProgram, sizeof(log), nullptr, log);
    qCWarning(LOG_BG, "Shader link error: %s", log);
    gl->glDeleteProgram(m_shaderProgram);
    m_shaderProgram = 0;
    gl->glDeleteShader(vs);
    gl->glDeleteShader(fs);
    return false;
  }

  gl->glDeleteShader(vs);
  gl->glDeleteShader(fs);
  return true;
}

void BackgroundImageDrawable::uploadTexture()
{
  if (!m_textureDirty || m_image.isNull()) return;

  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if (!ctx) return;
  QOpenGLFunctions *gl = ctx->functions();

  QImage glImage = m_image.mirrored(false, true).convertToFormat(QImage::Format_RGBA8888);
  if (glImage.isNull()) return;

  if (m_textureId) { gl->glDeleteTextures(1, &m_textureId); m_textureId = 0; }

  gl->glGenTextures(1, &m_textureId);
  gl->glBindTexture(GL_TEXTURE_2D, m_textureId);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                   glImage.width(), glImage.height(), 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, glImage.constBits());
  gl->glBindTexture(GL_TEXTURE_2D, 0);
  m_textureDirty = false;
}

void BackgroundImageDrawable::render(const Rendering::Camera &)
{
  if (m_image.isNull()) return;

  QOpenGLContext *ctx = QOpenGLContext::currentContext();
  if (!ctx) return;
  QOpenGLFunctions *gl = ctx->functions();

  // ---- Save GL state ----
  GLint savedProg = 0;
  gl->glGetIntegerv(GL_CURRENT_PROGRAM, &savedProg);
  GLboolean savedBlend = glIsEnabled(GL_BLEND);
  GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
  GLint savedBlendSrc = 0, savedBlendDst = 0;
  gl->glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrc);
  gl->glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDst);
  GLint savedActiveTex = 0;
  gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
  GLint savedTexBound0 = 0, savedTexBound1 = 0;
  gl->glActiveTexture(GL_TEXTURE0);
  gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexBound0);
  gl->glActiveTexture(GL_TEXTURE1);
  gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexBound1);
  GLint savedViewport[4];
  gl->glGetIntegerv(GL_VIEWPORT, savedViewport);
  int w = savedViewport[2], h = savedViewport[3];
  if (w <= 0 || h <= 0) { gl->glUseProgram(savedProg); return; }

  // Upload background image texture (only when dirty)
  uploadTexture();
  if (!m_textureId) { gl->glUseProgram(savedProg); return; }

  // Compile shader (once)
  if (!ensureShader()) { gl->glUseProgram(savedProg); return; }

  // ---- Step 1: Copy current framebuffer (molecule) to texture ----
  const bool fbSizeChanged = (m_fbCopyW != w || m_fbCopyH != h);
  if (fbSizeChanged) {
    if (m_fbCopyTexture) gl->glDeleteTextures(1, &m_fbCopyTexture);
    gl->glGenTextures(1, &m_fbCopyTexture);
    m_fbCopyW = w;
    m_fbCopyH = h;
  }
  gl->glActiveTexture(GL_TEXTURE1);
  gl->glBindTexture(GL_TEXTURE_2D, m_fbCopyTexture);
  if (fbSizeChanged) {
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
  if (gl->glGetError() != GL_NO_ERROR) {
    // Fallback for multisampled FBOs
    std::vector<unsigned char> pixels(w * h * 4);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (gl->glGetError() == GL_NO_ERROR)
      gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  }

  // ---- Step 2: Setup state ----
  gl->glDisable(GL_DEPTH_TEST);
  gl->glDisable(GL_BLEND);

  // ---- Step 3: Single draw with combined shader ----
  gl->glUseProgram(m_shaderProgram);

  // Bind background image to texture unit 0
  gl->glActiveTexture(GL_TEXTURE0);
  gl->glBindTexture(GL_TEXTURE_2D, m_textureId);
  gl->glUniform1i(gl->glGetUniformLocation(m_shaderProgram, "uBackground"), 0);

  // Bind framebuffer copy to texture unit 1
  gl->glActiveTexture(GL_TEXTURE1);
  gl->glBindTexture(GL_TEXTURE_2D, m_fbCopyTexture);
  gl->glUniform1i(gl->glGetUniformLocation(m_shaderProgram, "uFramebuffer"), 1);

  // Compute aspect-ratio UV transform: bgUV = (vUV - offset) / scale
  float bgScaleX = 1.0f, bgScaleY = 1.0f;
  float bgOffsetX = 0.0f, bgOffsetY = 0.0f;
  if (m_keepAspectRatio && m_image.width() > 0 && m_image.height() > 0 && h > 0) {
    float imgAspect = static_cast<float>(m_image.width()) / m_image.height();
    float vpAspect = static_cast<float>(w) / h;
    if (imgAspect > vpAspect) {
      float scale = vpAspect / imgAspect;
      bgScaleY = scale;
      bgOffsetY = (1.0f - scale) * 0.5f;
    } else if (imgAspect < vpAspect) {
      float scale = imgAspect / vpAspect;
      bgScaleX = scale;
      bgOffsetX = (1.0f - scale) * 0.5f;
    }
  }
  gl->glUniform2f(gl->glGetUniformLocation(m_shaderProgram, "uBgUVOffset"), bgOffsetX, bgOffsetY);
  gl->glUniform2f(gl->glGetUniformLocation(m_shaderProgram, "uBgUVScale"), bgScaleX, bgScaleY);

  // Draw fullscreen quad using VBO+VAO (required for OpenGL core profile)
  QOpenGLExtraFunctions *glx = ctx->extraFunctions();
  static const float quadVerts[] = { -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f };

  GLuint vao = 0, vbo = 0;
  glx->glGenVertexArrays(1, &vao);
  glx->glBindVertexArray(vao);
  gl->glGenBuffers(1, &vbo);
  gl->glBindBuffer(GL_ARRAY_BUFFER, vbo);
  gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
  gl->glEnableVertexAttribArray(0);
  gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  gl->glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  gl->glDisableVertexAttribArray(0);
  glx->glBindVertexArray(0);
  glx->glDeleteVertexArrays(1, &vao);
  gl->glDeleteBuffers(1, &vbo);
  gl->glBindBuffer(GL_ARRAY_BUFFER, 0);

  // ---- Restore GL state ----
  if (!savedDepthTest) gl->glDisable(GL_DEPTH_TEST);
  else gl->glEnable(GL_DEPTH_TEST);
  if (!savedBlend) gl->glDisable(GL_BLEND);
  else gl->glEnable(GL_BLEND);
  gl->glBlendFunc(savedBlendSrc, savedBlendDst);
  gl->glActiveTexture(GL_TEXTURE1);
  gl->glBindTexture(GL_TEXTURE_2D, savedTexBound1);
  gl->glActiveTexture(GL_TEXTURE0);
  gl->glBindTexture(GL_TEXTURE_2D, savedTexBound0);
  gl->glActiveTexture(savedActiveTex);
  gl->glUseProgram(savedProg);
}

void BackgroundImageDrawable::clear() { clearImage(); }

// ==================== BackgroundImageScenePlugin ====================

BackgroundImageScenePlugin::BackgroundImageScenePlugin(QObject *parent)
  : ScenePlugin(parent), m_activeIndex(-1), m_setupWidget(nullptr),
    m_keepAspectRatio(true) {}

BackgroundImageScenePlugin::~BackgroundImageScenePlugin() {}

void BackgroundImageScenePlugin::process(const QtGui::Molecule &,
                                          Rendering::GroupNode &node)
{
  if (m_activeIndex < 0 || m_activeIndex >= m_images.size()) return;
  auto *geo = new Rendering::GeometryNode;
  auto *drawable = new BackgroundImageDrawable;
  drawable->setImage(m_images[m_activeIndex].image);
  drawable->setKeepAspectRatio(m_keepAspectRatio);
  geo->addDrawable(drawable);
  node.addChild(geo);
}

QWidget *BackgroundImageScenePlugin::setupWidget()
{
  if (!m_setupWidget) {
    m_setupWidget = new QWidget;
    m_setupWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *layout = new QVBoxLayout(m_setupWidget);
    layout->setContentsMargins(0, 0, 0, 0);

    // Image list
    auto *listWidget = new QListWidget;
    listWidget->setObjectName("imageList");
    listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    listWidget->setMaximumHeight(100);
    layout->addWidget(listWidget);

    // Buttons: Load + Remove
    auto *btnLayout = new QHBoxLayout;
    auto *loadBtn = new QPushButton(tr("Add Images..."));
    auto *removeBtn = new QPushButton(tr("Remove"));
    btnLayout->addWidget(loadBtn);
    btnLayout->addWidget(removeBtn);
    layout->addLayout(btnLayout);

    // Aspect ratio checkbox
    auto *aspectCheck = new QCheckBox(tr("Keep aspect ratio"));
    aspectCheck->setObjectName("aspectRatioCheck");
    aspectCheck->setChecked(m_keepAspectRatio);
    layout->addWidget(aspectCheck);

    // Connections
    connect(loadBtn, &QPushButton::clicked, this, &BackgroundImageScenePlugin::loadImages);
    connect(removeBtn, &QPushButton::clicked, this, &BackgroundImageScenePlugin::removeImage);
    connect(listWidget, &QListWidget::currentRowChanged, this, &BackgroundImageScenePlugin::selectImage);
    connect(aspectCheck, &QCheckBox::toggled, this, &BackgroundImageScenePlugin::toggleAspectRatio);
  }
  return m_setupWidget;
}

void BackgroundImageScenePlugin::loadImages()
{
  QStringList files = QFileDialog::getOpenFileNames(
      m_setupWidget, tr("Open Background Images"), QString(),
      tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;All Files (*)"));
  if (files.isEmpty()) return;

  auto *listWidget = m_setupWidget->findChild<QListWidget*>("imageList");
  if (!listWidget) return;

  // Block signals to avoid triggering selectImage for each added item
  listWidget->blockSignals(true);

  for (const QString &fp : files) {
    QImage img(fp);
    if (img.isNull()) continue;

    ImageEntry entry;
    entry.path = fp;
    entry.image = img;
    m_images.append(entry);

    auto *item = new QListWidgetItem(QFileInfo(fp).fileName(), listWidget);
    item->setToolTip(fp);
  }

  listWidget->blockSignals(false);

  // Select the last added image
  if (!m_images.isEmpty()) {
    int lastRow = m_images.size() - 1;
    listWidget->setCurrentRow(lastRow);
    m_activeIndex = lastRow;
  }

  emit drawablesChanged();
}

void BackgroundImageScenePlugin::removeImage()
{
  auto *listWidget = m_setupWidget->findChild<QListWidget*>("imageList");
  if (!listWidget) return;

  int row = listWidget->currentRow();
  if (row < 0 || row >= m_images.size()) return;

  m_images.removeAt(row);
  delete listWidget->takeItem(row);

  // Adjust active index
  if (m_images.isEmpty()) {
    m_activeIndex = -1;
  } else if (row >= m_images.size()) {
    m_activeIndex = m_images.size() - 1;
    listWidget->setCurrentRow(m_activeIndex);
  } else {
    m_activeIndex = row;
    listWidget->setCurrentRow(row);
  }

  emit drawablesChanged();
}

void BackgroundImageScenePlugin::selectImage(int row)
{
  if (row < 0 || row >= m_images.size()) return;
  m_activeIndex = row;
  emit drawablesChanged();
}

void BackgroundImageScenePlugin::toggleAspectRatio(bool checked)
{
  m_keepAspectRatio = checked;
  emit drawablesChanged();
}

} // namespace Avogadro

// ==================== Plugin Factory ====================
namespace Avogadro {
namespace QtPlugins {
class BackgroundImageScenePluginFactory
  : public QObject, public QtGui::ScenePluginFactory
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "org.openchemistry.avogadro.ScenePluginFactory")
  Q_INTERFACES(Avogadro::QtGui::ScenePluginFactory)
public:
  explicit BackgroundImageScenePluginFactory(QObject *parent = nullptr) : QObject(parent) {}
  QtGui::ScenePlugin *createInstance(QObject *parent = nullptr) override {
    auto *obj = new Avogadro::BackgroundImageScenePlugin(parent);
    obj->setObjectName("Background Image");
    return obj;
  }
  QString identifier() const override { return "Background Image"; }
  QString description() const override {
    return "Displays a user-loaded image as the viewport background.";
  }
};
} // namespace QtPlugins
} // namespace Avogadro

#include "backgroundimageengine.moc"
