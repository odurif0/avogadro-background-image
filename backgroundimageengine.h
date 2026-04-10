/******************************************************************************
  Background Image Scene Plugin for Avogadro 2

  Displays a user-loaded image as the viewport background, with the molecule
  rendered on top. Uses Overlay2DPass with a composite shader to make empty
  (black) areas of the molecule framebuffer transparent.
******************************************************************************/

#ifndef BACKGROUNDIMAGESCENEPLUGIN_H
#define BACKGROUNDIMAGESCENEPLUGIN_H

#include <avogadro/qtgui/sceneplugin.h>
#include <avogadro/rendering/drawable.h>
#include <avogadro/rendering/groupnode.h>
#include <avogadro/rendering/geometrynode.h>

#include <QImage>
#include <QPointer>
#include <QString>

namespace Avogadro {

namespace Rendering { class Camera; }
namespace QtGui { class Molecule; }

class BackgroundImageDrawable : public Rendering::Drawable
{
public:
  BackgroundImageDrawable();
  ~BackgroundImageDrawable() override;

  void setImage(const QImage &image);
  void clearImage();
  void render(const Rendering::Camera &camera) override;
  void clear() override;

private:
  QImage m_image;
  unsigned int m_textureId;
  bool m_textureDirty;
  unsigned int m_fbCopyTexture;
  int m_fbCopyW, m_fbCopyH;
  unsigned int m_shaderProgram;
  bool m_shaderReady;

  void uploadTexture();
  bool ensureShader();

  Q_DISABLE_COPY(BackgroundImageDrawable)
};

class BackgroundImageScenePlugin : public QtGui::ScenePlugin
{
  Q_OBJECT
  Q_DISABLE_COPY(BackgroundImageScenePlugin)
public:
  explicit BackgroundImageScenePlugin(QObject *parent = nullptr);
  ~BackgroundImageScenePlugin() override;

  QString name() const override { return tr("Background Image"); }
  QString description() const override {
    return tr("Displays a user-loaded image as the viewport background.");
  }

  void process(const QtGui::Molecule &molecule,
               Rendering::GroupNode &node) override;
  QWidget *setupWidget() override;
  bool hasSetupWidget() const override { return true; }

private:
  QImage m_image;
  QString m_imagePath;
  QPointer<QWidget> m_setupWidget;
  void updateSettingsWidget();

private Q_SLOTS:
  void loadImage();
  void clearImageSlot();
};

} // namespace Avogadro

#endif // BACKGROUNDIMAGESCENEPLUGIN_H
