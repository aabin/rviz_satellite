/* Copyright 2014 Gareth Cross

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <unordered_map>
#include <QtGlobal>
#include <QImage>

#include <ros/ros.h>
#include <tf/transform_listener.h>

#include <OGRE/OgreManualObject.h>
#include <OGRE/OgreMaterialManager.h>
#include <OGRE/OgreSceneManager.h>
#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreTextureManager.h>
#include <OGRE/OgreImageCodec.h>

#include "rviz/frame_manager.h"
#include "rviz/ogre_helpers/grid.h"
#include "rviz/properties/float_property.h"
#include "rviz/properties/int_property.h"
#include "rviz/properties/property.h"
#include "rviz/properties/quaternion_property.h"
#include "rviz/properties/ros_topic_property.h"
#include "rviz/properties/vector_property.h"
#include "rviz/validate_floats.h"
#include "rviz/display_context.h"

#include "aerialmap_display.h"
#include "General.h"

namespace rviz
{
/**
 * @file
 * The sequence of events is rather complex due to the asynchronous nature of the tile texture updates, and the
 * different coordinate systems and frame transforms involved:
 *
 * The navSatFixCallback calls the updateCenterTile function, which then queries a texture update and calls
 * transformTileToMapFrame. The latter finds and stores the transform from the NavSatFix frame to the map-frame, to
 * which the tiles are rigidly attached by ENU convention and Mercator projection. On each frame, update() is called,
 * which calls transformMapTileToFixedFrame, which then transforms the tile-map from the map-frame to the fixed-frame.
 * Splitting this transform lookup is necessary to mitigate frame jitter.
 */

std::string const AerialMapDisplay::MAP_FRAME = "map";

AerialMapDisplay::AerialMapDisplay() : Display(), dirty_(false)
{
  topic_property_ =
      new RosTopicProperty("Topic", "", QString::fromStdString(ros::message_traits::datatype<sensor_msgs::NavSatFix>()),
                           "sensor_msgs::NavSatFix topic to subscribe to.", this, SLOT(updateTopic()));

  alpha_property_ =
      new FloatProperty("Alpha", 0.7, "Amount of transparency to apply to the map.", this, SLOT(updateAlpha()));
  alpha_ = alpha_property_->getValue().toFloat();
  alpha_property_->setMin(0);
  alpha_property_->setMax(1);
  alpha_property_->setShouldBeSaved(true);

  draw_under_property_ = new Property("Draw Behind", false,
                                      "Rendering option, controls whether or not the map is always"
                                      " drawn behind everything else.",
                                      this, SLOT(updateDrawUnder()));
  draw_under_property_->setShouldBeSaved(true);
  draw_under_ = draw_under_property_->getValue().toBool();

  // properties for map
  tile_url_property_ =
      new StringProperty("Object URI", "", "URL from which to retrieve map tiles.", this, SLOT(updateTileUrl()));
  tile_url_property_->setShouldBeSaved(true);
  tile_url_ = tile_url_property_->getStdString();

  QString const zoom_desc = QString::fromStdString("Zoom level (0 - " + std::to_string(maxZoom) + ")");
  zoom_property_ = new IntProperty("Zoom", 16, zoom_desc, this, SLOT(updateZoom()));
  zoom_property_->setShouldBeSaved(true);
  zoom_property_->setMin(0);
  zoom_property_->setMax(maxZoom);
  zoom_ = zoom_property_->getInt();

  QString const blocks_desc = QString::fromStdString("Adjacent blocks (0 - " + std::to_string(maxBlocks) + ")");
  blocks_property_ = new IntProperty("Blocks", 3, blocks_desc, this, SLOT(updateBlocks()));
  blocks_property_->setShouldBeSaved(true);
  blocks_property_->setMin(0);
  blocks_property_->setMax(maxBlocks);
  blocks_ = blocks_property_->getInt();
}

AerialMapDisplay::~AerialMapDisplay()
{
  unsubscribe();
  clearAll();
}

void AerialMapDisplay::onEnable()
{
  createTileObjects();
  subscribe();
}

void AerialMapDisplay::onDisable()
{
  unsubscribe();
  clearAll();
}

void AerialMapDisplay::subscribe()
{
  if (!isEnabled())
  {
    return;
  }

  if (!topic_property_->getTopic().isEmpty())
  {
    try
    {
      ROS_INFO("Subscribing to %s", topic_property_->getTopicStd().c_str());
      coord_sub_ = update_nh_.subscribe(topic_property_->getTopicStd(), 1, &AerialMapDisplay::navFixCallback, this);

      setStatus(StatusProperty::Ok, "Topic", "OK");
    }
    catch (ros::Exception& e)
    {
      setStatus(StatusProperty::Error, "Topic", QString("Error subscribing: ") + e.what());
    }
  }
}

void AerialMapDisplay::unsubscribe()
{
  coord_sub_.shutdown();
}

void AerialMapDisplay::updateAlpha()
{
  // if draw_under_ texture property changed, we need to
  //  - repaint textures
  // we don't need to
  //  - query textures
  //  - re-create tile grid geometry
  //  - update the center tile
  //  - update transforms

  auto const alpha = alpha_property_->getFloat();
  if (alpha == alpha_)
  {
    return;
  }

  alpha_ = alpha;

  if (!isEnabled())
  {
    return;
  }

  dirty_ = true;
}

void AerialMapDisplay::updateDrawUnder()
{
  // if draw_under_ texture property changed, we need to
  //  - repaint textures
  // we don't need to
  //  - query textures
  //  - re-create tile grid geometry
  //  - update the center tile
  //  - update transforms

  auto const draw_under = draw_under_property_->getValue().toBool();
  if (draw_under == draw_under_)
  {
    return;
  }

  draw_under_ = draw_under;

  if (!isEnabled())
  {
    return;
  }

  dirty_ = true;
}

void AerialMapDisplay::updateTileUrl()
{
  // if the tile url changed, we need to
  //  - query textures
  //  - repaint textures
  // we don't need to
  //  - re-create tile grid geometry
  //  - update the center tile
  //  - update transforms

  auto const tile_url = tile_url_property_->getStdString();
  if (tile_url == tile_url_)
  {
    return;
  }

  tile_url_ = tile_url;

  if (!isEnabled())
  {
    return;
  }

  requestTileTextures();
}

void AerialMapDisplay::updateZoom()
{
  // if the zoom changed, we need to
  //  - re-create tile grid geometry
  //  - update the center tile
  //  - query textures
  //  - repaint textures
  //  - update transforms

  auto const zoom = zoom_property_->getInt();
  if (zoom == zoom_)
  {
    return;
  }

  zoom_ = zoom;

  if (!isEnabled())
  {
    return;
  }

  createTileObjects();

  if (ref_fix_)
  {
    updateCenterTile(ref_fix_);
  }
}

void AerialMapDisplay::updateBlocks()
{
  // if the number of blocks changed, we need to
  //  - re-create tile grid geometry
  //  - query textures
  //  - repaint textures
  // we don't need to
  //  - update the center tile
  //  - update transforms

  auto const blocks = blocks_property_->getInt();
  if (blocks == blocks_)
  {
    return;
  }

  blocks_ = blocks;

  if (!isEnabled())
  {
    return;
  }

  createTileObjects();
  requestTileTextures();
}

void AerialMapDisplay::updateTopic()
{
  // if the NavSat topic changes, we reset everything

  if (!isEnabled())
  {
    return;
  }

  unsubscribe();
  clearAll();
  createTileObjects();
  subscribe();
}

void AerialMapDisplay::clearAll()
{
  ref_fix_ = nullptr;
  lastCenterTile_ = boost::none;
  destroyTileObjects();

  setStatus(StatusProperty::Warn, "Message", "No map received yet");
}

void AerialMapDisplay::destroyTileObjects()
{
  for (MapObject& obj : objects_)
  {
    // destroy object
    scene_node_->detachObject(obj.object);
    scene_manager_->destroyManualObject(obj.object);

    // destroy material
    if (!obj.material.isNull())
    {
      Ogre::MaterialManager::getSingleton().remove(obj.material->getName());
    }
  }
  objects_.clear();
}

void AerialMapDisplay::createTileObjects()
{
  if (not objects_.empty())
  {
    destroyTileObjects();
  }

  for (int block = 0; block < (2 * blocks_ + 1) * (2 * blocks_ + 1); ++block)
  {
    // generate an unique name
    static int count = 0;
    std::string const name_suffix = std::to_string(count);
    ++count;

    // one material per texture
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(
        "satellite_material_" + name_suffix, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    material->setReceiveShadows(false);
    material->getTechnique(0)->setLightingEnabled(false);
    material->setDepthBias(-16.0f, 0.0f);
    material->setCullingMode(Ogre::CULL_NONE);
    material->setDepthWriteEnabled(false);

    // create texture and initialize it
    Ogre::TextureUnitState* tex_unit = material->getTechnique(0)->getPass(0)->createTextureUnitState();
    tex_unit->setTextureFiltering(Ogre::TFO_BILINEAR);

    // create an object
    Ogre::ManualObject* obj = scene_manager_->createManualObject("satellite_object_" + name_suffix);
    obj->setVisible(false);
    scene_node_->attachObject(obj);

    assert(!material.isNull());
    objects_.emplace_back(obj, material);
  }
}

void AerialMapDisplay::update(float, float)
{
  if (not ref_fix_ or not lastCenterTile_)
  {
    return;
  }

  // update tiles, if necessary
  assembleScene();
  // transform scene object into fixed frame
  transformMapTileToFixedFrame();
}

void AerialMapDisplay::navFixCallback(sensor_msgs::NavSatFixConstPtr const& msg)
{
  // TODO: even if the center tile does not change, our lat/lon and tf relationship could have been changed? so update
  // the transform either way?
  updateCenterTile(msg);

  setStatus(StatusProperty::Ok, "Message", "NavSatFix okay");
}

void AerialMapDisplay::updateCenterTile(sensor_msgs::NavSatFixConstPtr const& msg)
{
  if (!isEnabled())
  {
    return;
  }

  // check if update is necessary
  auto const tileCoordinates = fromWGSCoordinate({ msg->latitude, msg->longitude }, zoom_);
  TileId const newCenterTileID{ tile_url_, tileCoordinates, zoom_ };
  bool const centerTileChanged = (!lastCenterTile_ || !(newCenterTileID == *lastCenterTile_));

  if (not centerTileChanged)
  {
    return;
  }

  ROS_DEBUG_NAMED("rviz_satellite", "Updating center tile");

  lastCenterTile_ = newCenterTileID;
  ref_fix_ = msg;

  requestTileTextures();
  transformTileToMapFrame();
}

void AerialMapDisplay::requestTileTextures()
{
  if (!isEnabled())
  {
    return;
  }

  if (tile_url_.empty())
  {
    setStatus(StatusProperty::Error, "TileRequest", "Tile URL is not set");
    return;
  }

  if (not lastCenterTile_)
  {
    setStatus(StatusProperty::Error, "Message", "No NavSatFix received yet");
    return;
  }

  try
  {
    tileCache_.request({ *lastCenterTile_, blocks_ });
    dirty_ = true;
  }
  catch (std::exception const& e)
  {
    setStatus(StatusProperty::Error, "TileRequest", QString(e.what()));
    return;
  }
}

void AerialMapDisplay::checkRequestErrorRate()
{
  // the following error rate thresholds are randomly chosen
  float const errorRate = tileCache_.getTileServerErrorRate(tile_url_);
  if (errorRate > 0.95)
  {
    setStatus(StatusProperty::Level::Error, "TileRequest", "Few or no tiles received");
  }
  else if (errorRate > 0.3)
  {
    setStatus(StatusProperty::Level::Warn, "TileRequest",
              "Not all requested tiles have been received. Possibly the server is throttling?");
  }
  else
  {
    setStatus(StatusProperty::Level::Ok, "TileRequest", "OK");
  }
}

void AerialMapDisplay::assembleScene()
{
  if (!isEnabled() || !dirty_ || !lastCenterTile_)
  {
    return;
  }

  if (objects_.empty())
  {
    ROS_ERROR_THROTTLE_NAMED(5, "rviz_satellite", "No objects to draw on, call createTileObjects() first!");
    return;
  }

  dirty_ = false;

  Area area(*lastCenterTile_, blocks_);

  TileCacheGuard guard(tileCache_);

  bool loadedAllTiles = true;

  auto it = objects_.begin();
  for (int xx = area.leftTop.x; xx <= area.rightBottom.x; ++xx)
  {
    for (int yy = area.leftTop.y; yy <= area.rightBottom.y; ++yy)
    {
      auto obj = it->object;
      auto& material = it->material;
      assert(!material.isNull());
      ++it;

      TileId const toFind{ lastCenterTile_->tileServer, { xx, yy }, lastCenterTile_->zoom };

      OgreTile const* tile = tileCache_.ready(toFind);
      if (!tile)
      {
        // don't show tiles with old textures
        obj->setVisible(false);
        loadedAllTiles = false;
        continue;
      }

      obj->setVisible(true);

      // update texture
      Ogre::TextureUnitState* tex_unit = material->getTechnique(0)->getPass(0)->getTextureUnitState(0);
      tex_unit->setTextureName(tile->texture->getName());

      // configure depth & alpha properties
      if (alpha_ >= 0.9998)
      {
        material->setDepthWriteEnabled(!draw_under_);
        material->setSceneBlending(Ogre::SBT_REPLACE);
      }
      else
      {
        material->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        material->setDepthWriteEnabled(false);
      }

      if (draw_under_)
      {
        // render under everything else
        obj->setRenderQueueGroup(Ogre::RENDER_QUEUE_3);
      }
      else
      {
        obj->setRenderQueueGroup(Ogre::RENDER_QUEUE_MAIN);
      }

      tex_unit->setAlphaOperation(Ogre::LBX_SOURCE1, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, alpha_);

      // tile width/ height in meter
      double const tile_w_h_m = getTileWH(ref_fix_->latitude, zoom_);

      // Note: In the following we will do two things:
      //
      // * We flip the position's y coordinate.
      // * We flip the texture's v coordinate.
      //
      // For more explanation see the function transformAerialMap()

      // The center tile has the coordinates left-bot = (0,0) and right-top = (1,1) in the AerialMap frame.
      double const x = (xx - lastCenterTile_->coord.x) * tile_w_h_m;
      // flip the y coordinate because we need to flip the tiles to align the tile's frame with the ENU "map" frame
      double const y = -(yy - lastCenterTile_->coord.y) * tile_w_h_m;

      // create a quad for this tile
      // note: We have to recreate the vertices and cannot reuse the old vertices: tile_w_h_m depends on the latitude
      obj->clear();

      obj->begin(material->getName(), Ogre::RenderOperation::OT_TRIANGLE_LIST);

      // We assign the Ogre texture coordinates in a way so that we flip the
      // texture along the v coordinate. For example, we assign the bottom left
      //
      // Note that the Ogre texture coordinate system is: (0,0) = top left of the loaded image and (1,1) = bottom right
      // of the loaded image

      // bottom left
      obj->position(x, y, 0.0f);
      obj->textureCoord(0.0f, 0.0f);
      obj->normal(0.0f, 0.0f, 1.0f);

      // top right
      obj->position(x + tile_w_h_m, y + tile_w_h_m, 0.0f);
      obj->textureCoord(1.0f, 1.0f);
      obj->normal(0.0f, 0.0f, 1.0f);

      // top left
      obj->position(x, y + tile_w_h_m, 0.0f);
      obj->textureCoord(0.0f, 1.0f);
      obj->normal(0.0f, 0.0f, 1.0f);

      // bottom left
      obj->position(x, y, 0.0f);
      obj->textureCoord(0.0f, 0.0f);
      obj->normal(0.0f, 0.0f, 1.0f);

      // bottom right
      obj->position(x + tile_w_h_m, y, 0.0f);
      obj->textureCoord(1.0f, 0.0f);
      obj->normal(0.0f, 0.0f, 1.0f);

      // top right
      obj->position(x + tile_w_h_m, y + tile_w_h_m, 0.0f);
      obj->textureCoord(1.0f, 1.0f);
      obj->normal(0.0f, 0.0f, 1.0f);

      obj->end();
    }
  }

  // since not all tiles were loaded yet, this function has to be called again
  if (!loadedAllTiles)
  {
    dirty_ = true;
  }

  tileCache_.purge({ *lastCenterTile_, blocks_ });

  checkRequestErrorRate();
}

/**
 * Calculate the tile width/ height in meter
 */
double AerialMapDisplay::getTileWH(double const latitude, int const zoom) const
{
  // this constant origins from how the base resolution is calculated
  //
  // see https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
  //
  // TODO: actually this not needed and could be removed from both formulas, since they cancel out each other;
  // it origins from most tile map applications directly rendering images with pixel dimensions;
  // in here we have OpenGL, pixel do not matter, only meters
  int constexpr tile_w_h_px = 256;

  // meter/pixel
  auto const resolution = zoomToResolution(latitude, zoom);
  // gives tile size (with and height) in meter
  double const tile_w_h_m = tile_w_h_px * resolution;
  return tile_w_h_m;
}

void AerialMapDisplay::transformTileToMapFrame()
{
  if (not ref_fix_)
  {
    ROS_FATAL_THROTTLE_NAMED(2, "rviz_satellite", "ref_fix_  not set, can't create transforms");
    return;
  }

  // We will use three frames in this function:
  //
  // * The frame from the NavSatFix message. It is rigidly attached to the robot.
  // * The ENU world frame "map".
  // * The frame of the tiles. We assume that the tiles are in a frame where x points eastwards and y southwards (ENU).
  // This
  //   frame is used by OSM and Google Maps, see https://en.wikipedia.org/wiki/Web_Mercator_projection and
  //   https://developers.google.com/maps/documentation/javascript/coordinates.

  auto const current_fixed_frame = context_->getFrameManager()->getFixedFrame();

  // orientation of NavSatFix frame w.r.t. the map frame (unused)
  Ogre::Quaternion o_navsat_map;
  // translation of NavSatFix frame w.r.t. the map frame
  Ogre::Vector3 t_navsat_map;

  std::string error;
  bool const gotTransform =
      getMapTransform(ref_fix_->header.frame_id, ref_fix_->header.stamp, t_navsat_map, o_navsat_map, error);
  if (not gotTransform)
  {
    setStatus(StatusProperty::Error, "Transform", QString::fromStdString(error));
    return;
  }

  auto const centerTile = fromWGSCoordinate<double>({ ref_fix_->latitude, ref_fix_->longitude }, zoom_);

  // In assembleScene() we shift the AerialMap so that the center tile's left-bottom corner has the coordinate (0,0).
  // Therefore we can calculate the NavSatFix coordinate (in the AerialMap frame) by just looking at the fractional part
  // of the coordinate. That is we calculate the offset from the left bottom corner of the center tile.
  auto const centerTileOffsetX = centerTile.x - std::floor(centerTile.x);
  // In assembleScene() the tiles are created so that the texture is flipped along the y coordinate. Since we want to
  // calculate the positions of the center tile, we also need to flip the texture's v coordinate here.
  auto const centerTileOffsetY = 1 - (centerTile.y - std::floor(centerTile.y));

  double const tile_w_h_m = getTileWH(ref_fix_->latitude, zoom_);
  ROS_DEBUG_NAMED("rviz_satellite", "Tile resolution is %.1fm", tile_w_h_m);

  auto const translationAerialMapToNavSatFix =
      Ogre::Vector3(centerTileOffsetX * tile_w_h_m, centerTileOffsetY * tile_w_h_m, 0);
  auto const translationNavSatFixToAerialMap = -translationAerialMapToNavSatFix;

  t_centertile_map = t_navsat_map + translationNavSatFixToAerialMap;
}

bool AerialMapDisplay::getMapTransform(std::string const& query_frame, ros::Time const& timestamp,
                                       Ogre::Vector3& position, Ogre::Quaternion& orientation, std::string& error)
{
  // Unfortunately the FrameManager API does not allow looking up arbitrary frame transforms, only towards the currently
  // selected fixed-frame. To get the transform, we split the transform into two: frame_id to fixed-frame and map-frame
  // to fixed-frame. It would be easier to work with (tf2) Transforms here and a tf2 buffer, but the FrameManager has
  // its own cache and logic one should use. However, this creates the overhead to rotate and translate a bit manual
  // here ..

  // orientation of the query-frame w.r.t. fixed-frame
  Ogre::Quaternion o_query_fixed;
  // translation of the query-frame w.r.t. fixed-frame
  Ogre::Vector3 t_query_fixed;
  // orientation of the map-frame w.r.t. fixed-frame
  Ogre::Quaternion o_map_fixed;
  // translation of the map-frame w.r.t. fixed-frame
  Ogre::Vector3 t_map_fixed;

  // get first transform (query-frame to fixed-frame)
  if (!context_->getFrameManager()->getTransform(query_frame, timestamp, t_query_fixed, o_query_fixed))
  {
    // check error
    if (not context_->getFrameManager()->transformHasProblems(query_frame, timestamp, error))
    {
      error = "Could not transform from [" + query_frame + "] to Fixed Frame for an unknown reason";
    }

    return false;
  }

  // get second transform (map-frame to fixed-frame)
  if (!context_->getFrameManager()->getTransform(MAP_FRAME, timestamp, t_map_fixed, o_map_fixed))
  {
    // check error
    if (not context_->getFrameManager()->transformHasProblems(query_frame, timestamp, error))
    {
      error = "Could not transform from [" + MAP_FRAME + "] to Fixed Frame for an unknown reason";
    }

    return false;
  }

  // this is a bit cryptic, but that's how it is with transforms ;)
  orientation = o_map_fixed.Inverse() * o_query_fixed;
  position = o_map_fixed.Inverse() * t_query_fixed + o_map_fixed.Inverse() * -t_map_fixed;

  return true;
}

void AerialMapDisplay::transformMapTileToFixedFrame()
{
  // orientation of the fixed-frame w.r.t. the map-frame
  Ogre::Quaternion o_fixed_map;
  // translation of the fixed-frame w.r.t. the map-frame
  Ogre::Vector3 t_fixed_map;

  // get transform between map-frame and fixed-frame from the FrameManager
  if (context_->getFrameManager()->getTransform(MAP_FRAME, ros::Time(), t_fixed_map, o_fixed_map))
  {
    setStatus(::rviz::StatusProperty::Ok, "Transform", "Transform OK");

    // the translation of the tile w.r.t. the fixed-frame
    auto const t_centertile_fixed = t_fixed_map + o_fixed_map * t_centertile_map;
    scene_node_->setPosition(t_centertile_fixed);
    scene_node_->setOrientation(o_fixed_map);
  }
  else
  {
    // display error
    std::string error;
    if (context_->getFrameManager()->transformHasProblems(MAP_FRAME, ros::Time(), error))
    {
      setStatus(::rviz::StatusProperty::Error, "Transform", QString::fromStdString(error));
    }
    else
    {
      setStatus(::rviz::StatusProperty::Error, "Transform",
                QString::fromStdString("Could not transform from [" + MAP_FRAME + "] to Fixed Frame [" +
                                       fixed_frame_.toStdString() + "] for an unknown reason"));
    }
  }
}

void AerialMapDisplay::reset()
{
  Display::reset();
  // unsub,clear,resub
  updateTopic();
}

}  // namespace rviz

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(rviz::AerialMapDisplay, rviz::Display)
