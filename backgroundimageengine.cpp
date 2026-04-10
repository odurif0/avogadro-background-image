/******************************************************************************
  Background Image Scene Plugin for Avogadro 2

  Rendering technique:
    Uses Overlay2DPass (renders last, after all molecule geometry).
    1. Copy the current framebuffer to a texture (contains molecule)
    2. Draw the user's background image fullscreen
    3. Redraw the saved framebuffer on top using a GLSL shader
       that makes black (empty) areas transparent
******************************************************************************/

#include "backgroundimageengine.h"

#include <avogadro/rendering/camera.h>

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(LOG_BG, "avogadro.plugin.backgroundimage")

namespace Avogadro {

// ==================== BackgroundImageDrawable ====================

static const char *compositeVS =
  "attribute vec2 aPos;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
  "  vUV = aPos * 0.5 + 0.5;\n"
  "}\n";

static const char *compositeFS =
  "uniform sampler2D uTex;\n"
  "varying vec2 vUV;\n"
  "void main() {\n"
  "  vec4 c = texture2D(uTex, vUV);\n"
  "  float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
  "  float alpha = smoothstep(0.015, 0.06, lum);\n"
  "  gl_FragColor = vec4(c.rgb, alpha);\n"
  "}\n";

BackgroundImageDrawable::BackgroundImageDrawable()
  : m_textureId(0), m_textureDirty(true),
    m_fbCopyTexture(0), m_fbCopyW(0), m_fbCopyH(0),
    m_shaderProgram(0), m_shaderReady(false)
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

  GLuint vs = compileShader(gl, GL_VERTEX_SHADER, compositeVS);
  GLuint fs = compileShader(gl, GL_FRAGMENT_SHADER, compositeFS);
  if (!vs || !fs) return false;

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

  QImage glImage = m_image.flipped(Qt::Vertical).convertToFormat(QImage::Format_RGBA8888);
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

  // ---- Save entire GL state ----
  GLint savedProg = 0;
  gl->glGetIntegerv(GL_CURRENT_PROGRAM, &savedProg);
  GLboolean savedBlend = glIsEnabled(GL_BLEND);
  GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
  GLint savedBlendSrc = 0, savedBlendDst = 0;
  gl->glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrc);
  gl->glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDst);
  GLint savedActiveTex = 0;
  gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
  GLint savedTexBound = 0;
  gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTexBound);
  GLint savedViewport[4];
  gl->glGetIntegerv(GL_VIEWPORT, savedViewport);
  int w = savedViewport[2], h = savedViewport[3];
  if (w <= 0 || h <= 0) { gl->glUseProgram(savedProg); return; }

  // Upload background image texture (only when dirty)
  uploadTexture();
  if (!m_textureId) { gl->glUseProgram(savedProg); return; }

  // Compile composite shader (once)
  if (!ensureShader()) { gl->glUseProgram(savedProg); return; }

  // ---- Step 1: Copy current framebuffer (molecule) to texture ----
  const bool fbSizeChanged = (m_fbCopyW != w || m_fbCopyH != h);
  if (fbSizeChanged) {
    if (m_fbCopyTexture) gl->glDeleteTextures(1, &m_fbCopyTexture);
    gl->glGenTextures(1, &m_fbCopyTexture);
    m_fbCopyW = w;
    m_fbCopyH = h;
  }
  gl->glBindTexture(GL_TEXTURE_2D, m_fbCopyTexture);
  if (fbSizeChanged) {
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  gl->glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w, h, 0);

  // ---- Step 2: Setup 2D state ----
  gl->glUseProgram(0);
  gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
  gl->glDisable(GL_DEPTH_TEST);
  gl->glEnable(GL_BLEND);
  gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl->glActiveTexture(GL_TEXTURE0);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  // ---- Step 3: Draw background image fullscreen ----
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glEnable(GL_TEXTURE_2D);
  gl->glBindTexture(GL_TEXTURE_2D, m_textureId);
  glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
  glEnd();
  glDisable(GL_TEXTURE_2D);

  // ---- Step 4: Draw framebuffer on top with composite shader ----
  gl->glUseProgram(m_shaderProgram);
  gl->glActiveTexture(GL_TEXTURE0);
  gl->glBindTexture(GL_TEXTURE_2D, m_fbCopyTexture);
  GLint texLoc = gl->glGetUniformLocation(m_shaderProgram, "uTex");
  gl->glUniform1i(texLoc, 0);

  static const float quadVerts[] = { -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f };
  gl->glEnableVertexAttribArray(0);
  gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quadVerts);
  gl->glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  gl->glDisableVertexAttribArray(0);

  // ---- Restore GL state ----
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();

  if (!savedDepthTest) gl->glDisable(GL_DEPTH_TEST);
  else gl->glEnable(GL_DEPTH_TEST);
  if (!savedBlend) gl->glDisable(GL_BLEND);
  else gl->glEnable(GL_BLEND);
  gl->glBlendFunc(savedBlendSrc, savedBlendDst);
  gl->glActiveTexture(savedActiveTex);
  gl->glBindTexture(GL_TEXTURE_2D, savedTexBound);
  gl->glUseProgram(savedProg);
}

void BackgroundImageDrawable::clear() { clearImage(); }

// ==================== BackgroundImageScenePlugin ====================

BackgroundImageScenePlugin::BackgroundImageScenePlugin(QObject *parent)
  : ScenePlugin(parent), m_setupWidget(nullptr) {}

BackgroundImageScenePlugin::~BackgroundImageScenePlugin() {}

void BackgroundImageScenePlugin::process(const QtGui::Molecule &,
                                          Rendering::GroupNode &node)
{
  if (m_image.isNull()) return;
  auto *geo = new Rendering::GeometryNode;
  auto *drawable = new BackgroundImageDrawable;
  drawable->setImage(m_image);
  geo->addDrawable(drawable);
  node.addChild(geo);
}

QWidget *BackgroundImageScenePlugin::setupWidget()
{
  if (!m_setupWidget) {
    m_setupWidget = new QWidget;
    auto *layout = new QVBoxLayout(m_setupWidget);
    auto *label = new QLabel(m_imagePath.isEmpty()
                             ? tr("No image loaded")
                             : QFileInfo(m_imagePath).fileName());
    label->setObjectName("imagePathLabel");
    label->setWordWrap(true);
    if (!m_imagePath.isEmpty()) label->setToolTip(m_imagePath);

    auto *btnLayout = new QHBoxLayout;
    auto *loadBtn = new QPushButton(tr("Load Image..."));
    auto *clearBtn = new QPushButton(tr("Clear Image"));
    btnLayout->addWidget(loadBtn);
    btnLayout->addWidget(clearBtn);
    layout->addWidget(label);
    layout->addLayout(btnLayout);

    connect(loadBtn, &QPushButton::clicked, this, &BackgroundImageScenePlugin::loadImage);
    connect(clearBtn, &QPushButton::clicked, this, &BackgroundImageScenePlugin::clearImageSlot);
  }
  return m_setupWidget;
}

void BackgroundImageScenePlugin::loadImage()
{
  QString fp = QFileDialog::getOpenFileName(m_setupWidget, tr("Open Background Image"),
      m_imagePath, tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;All Files (*)"));
  if (fp.isEmpty()) return;
  QImage img(fp);
  if (img.isNull()) return;
  m_imagePath = fp;
  m_image = img;
  updateSettingsWidget();
  emit drawablesChanged();
}

void BackgroundImageScenePlugin::clearImageSlot()
{
  m_imagePath.clear();
  m_image = QImage();
  updateSettingsWidget();
  emit drawablesChanged();
}

void BackgroundImageScenePlugin::updateSettingsWidget()
{
  if (!m_setupWidget) return;
  auto *label = m_setupWidget->findChild<QLabel*>("imagePathLabel");
  if (label) {
    if (m_imagePath.isEmpty()) {
      label->setText(tr("No image loaded"));
      label->setToolTip(QString());
    } else {
      label->setText(QFileInfo(m_imagePath).fileName());
      label->setToolTip(m_imagePath);
    }
  }
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
