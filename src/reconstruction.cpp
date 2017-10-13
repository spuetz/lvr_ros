/*
 * UOS-ROS packages - Robot Operating System code by the University of Osnabrück
 * Copyright (C) 2013 University of Osnabrück
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * reconstruction.cpp
 *
 * Author: Sebastian Pütz <spuetz@uos.de>,
 *
 */

#include <iostream>
#include <memory>

using std::make_shared;


#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "lvr_ros/reconstruction.h"
#include "lvr_ros/conversions.h"

#include <lvr/io/PLYIO.hpp>
#include <lvr/config/lvropenmp.hpp>
#include <lvr/geometry/Matrix4.hpp>
#include <lvr/texture/Texture.hpp>
#include <lvr/texture/Transform.hpp>
#include <lvr/texture/Texturizer.hpp>
#include <lvr/texture/Statistics.hpp>
#include <lvr/geometry/QuadricVertexCosts.hpp>

#include <lvr2/geometry/HalfEdgeMesh.hpp>
#include <lvr2/geometry/Vector.hpp>
#include <lvr2/geometry/Point.hpp>
#include <lvr2/geometry/Normal.hpp>
#include <lvr2/algorithm/FinalizeAlgorithm.hpp>
#include <lvr2/geometry/BoundingBox.hpp>
#include <lvr2/algorithm/Planar.hpp>
#include <lvr2/algorithm/NormalAlgorithms.hpp>
#include <lvr2/algorithm/CleanupAlgorithms.hpp>
#include <lvr2/algorithm/ClusterAlgorithms.hpp>
#include <lvr2/algorithm/ClusterPainter.hpp>
#include <lvr2/geometry/Handles.hpp>
#include <lvr2/util/ClusterBiMap.hpp>

#include <lvr2/reconstruction/AdaptiveKSearchSurface.hpp>
#include <lvr2/reconstruction/BilinearFastBox.hpp>
#include <lvr2/reconstruction/FastReconstruction.hpp>
#include <lvr2/reconstruction/PointsetSurface.hpp>
#include <lvr2/reconstruction/SearchTree.hpp>
#include <lvr2/reconstruction/SearchTreeFlann.hpp>
#include <lvr2/reconstruction/HashGrid.hpp>
#include <lvr2/reconstruction/PointsetGrid.hpp>
#include <lvr2/io/PointBuffer.hpp>
#include <lvr2/util/Factories.hpp>
#include <lvr2/util/Panic.hpp>

namespace lvr_ros
{

/**********************************************************************************************************************/
// Constructor

Reconstruction::Reconstruction()
    : as_(node_handle, "reconstruction", boost::bind(&Reconstruction::reconstruct, this, _1), false)
{
    ros::NodeHandle nh("~");

    cloud_subscriber = node_handle.subscribe(
        "/pointcloud",
        1,
        &Reconstruction::pointCloudCallback,
        this
    );
    mesh_publisher = node_handle.advertise<mesh_msgs::TriangleMeshStamped>("/mesh", 1);
    mesh_geometry_publisher = node_handle.advertise<mesh_msgs::MeshGeometryStamped>("/mesh_geometry", 1);

    // Setup dynamic reconfigure
    reconfigure_server_ptr = DynReconfigureServerPtr(new DynReconfigureServer(nh));
    callback_type = boost::bind(&Reconstruction::reconfigureCallback, this, _1, _2);
    reconfigure_server_ptr->setCallback(callback_type);

    // Start action server
    as_.start();

    // Start services
    srv_get_geometry_ = node_handle.advertiseService("get_geometry", &Reconstruction::service_getGeometry, this);
    srv_get_materials_ = node_handle.advertiseService("get_materials", &Reconstruction::service_getMaterials, this);
    srv_get_texture_ = node_handle.advertiseService("get_texture", &Reconstruction::service_getTexture, this);
    srv_get_uuid_ = node_handle.advertiseService("get_uuid", &Reconstruction::service_getUUID, this);
    srv_get_vertex_colors_ = node_handle.advertiseService(
        "get_vertex_colors",
        &Reconstruction::service_getVertexColors,
        this
    );

}

/**********************************************************************************************************************/
// Actions & Services

void Reconstruction::reconstruct(const lvr_ros::ReconstructGoalConstPtr& goal)
{
    try
    {
        lvr_ros::ReconstructResult result;
        createMeshMessageFromPointCloud(goal->cloud, result.mesh);
        result.uuid = cache_uuid;
        as_.setSucceeded(result, "Published mesh.");
    }
    catch(std::exception& e)
    {
        ROS_ERROR_STREAM("Error: " << e.what());
        as_.setAborted();
    }
}

bool Reconstruction::service_getGeometry(
    lvr_ros::GetGeometry::Request& req,
    lvr_ros::GetGeometry::Response& res
)
{
    if (req.uuid != cache_uuid)
    {
        return false;
    }
    res.mesh_geometry_stamped = cache_mesh_geometry_stamped;
    return true;
}

bool Reconstruction::service_getMaterials(
    lvr_ros::GetMaterials::Request& req,
    lvr_ros::GetMaterials::Response& res
)
{
    if (req.uuid != cache_uuid)
    {
        return false;
    }
    res.mesh_materials_stamped = cache_mesh_materials_stamped;
    return true;
}

bool Reconstruction::service_getTexture(
    lvr_ros::GetTexture::Request& req,
    lvr_ros::GetTexture::Response& res
)
{
    if (req.uuid != cache_uuid || req.texture_index > cache_textures.size() - 1)
    {
        return false;
    }
    res.texture = cache_textures.at(req.texture_index);
    return true;
}

bool Reconstruction::service_getVertexColors(
    lvr_ros::GetVertexColors::Request& req,
    lvr_ros::GetVertexColors::Response& res
)
{
    if (req.uuid != cache_uuid)
    {
        return false;
    }
    res.mesh_vertex_colors_stamped = cache_mesh_vertex_colors_stamped;
    return true;
}

bool Reconstruction::service_getUUID(
    lvr_ros::GetUUID::Request& req,
    lvr_ros::GetUUID::Response& res
)
{
    res.uuid = cache_uuid;
    return true;
}

/**********************************************************************************************************************/
// Callbacks

void Reconstruction::pointCloudCallback(const sensor_msgs::PointCloud2::ConstPtr& cloud)
{
    mesh_msgs::TriangleMeshStamped mesh;
    if (!createMeshMessageFromPointCloud(*cloud, mesh))
    {
        ROS_ERROR_STREAM("Error in PointCloud callback");
    }

    // Reconstruction is done, publish TriangleMesh (deprecated!)
    mesh_publisher.publish(mesh);
    // .. and also publish MeshGeometry (new! use this)
    mesh_geometry_publisher.publish(cache_mesh_geometry_stamped);
}

void Reconstruction::reconfigureCallback(lvr_ros::ReconstructionConfig& config, uint32_t level)
{
    this->config = config;
}

/**********************************************************************************************************************/
// Reconstruction Logic

bool Reconstruction::createMeshMessageFromPointCloud(
    const sensor_msgs::PointCloud2& cloud,
    mesh_msgs::TriangleMeshStamped& mesh_msg
)
{

    /*
     * This method will generate
     *   - a TriangleMesh message
     *       - this message will be published in the callback or action function that is calling this method
     *   - a MeshGeometry message and all corresponding MeshAttribute messages
     *       - these messages will be cached and will be available via a service
     *
     * Please note: For future versions, it is not intended to keep both messages around. TriangleMesh will be
     * discontinued in favor of the new message structure. To ensure a smooth transition between both APIs, this
     * version of LVR_ROS will be able to generate both messages.
     */


    PointBufferPtr point_buffer_ptr(new PointBuffer);
    lvr2::MeshBufferPtr<Vec> mesh_buffer_ptr(new lvr2::MeshBuffer<Vec>);

    if (!lvr_ros::fromPointCloud2ToPointBuffer(cloud, *point_buffer_ptr))
    {
        ROS_ERROR_STREAM(
            "Could not convert point cloud from \"sensor_msgs::PointCloud2\" "
            "to \"lvr::PointBuffer\"!"
        );
        return false;
    }
    if (!createMeshBufferFromPointBuffer(point_buffer_ptr, mesh_buffer_ptr))
    {
        ROS_ERROR_STREAM("Reconstruction failed!");
        return false;
    }
    if (!lvr_ros::fromMeshBufferToTriangleMesh(mesh_buffer_ptr->toOldBuffer(), mesh_msg.mesh))
    {
        ROS_ERROR_STREAM(
            "Could not convert point cloud from \"lvr::MeshBuffer\" "
            "to \"mesh_msgs::TriangleMeshStamped\"!"
        );
        return false;
    }
    if (!lvr_ros::fromMeshBufferToMeshMessages(
            mesh_buffer_ptr,
            cache_mesh_geometry_stamped.mesh_geometry,
            cache_mesh_materials_stamped.mesh_materials,
            cache_mesh_vertex_colors_stamped.mesh_vertex_colors,
            cache_textures
    ))
    {
        ROS_ERROR_STREAM("Could not convert \"lvr2::MeshBuffer\" to mesh messages!");
        return false;
    }

    // Setting header frame and stamp for TriangleMesh
    mesh_msg.header.frame_id = cloud.header.frame_id;
    mesh_msg.header.stamp = cloud.header.stamp;



    // The following segment will update new MeshGeometry and MeshAttribute messages in cache
    // These messages will be available via action/service
    cache_initialized = true;

    // Setting header frame and stamp
    cache_mesh_geometry_stamped.header.frame_id = cloud.header.frame_id;
    cache_mesh_geometry_stamped.header.stamp = cloud.header.stamp;
    cache_mesh_materials_stamped.header.frame_id = cloud.header.frame_id;
    cache_mesh_materials_stamped.header.stamp = cloud.header.stamp;
    cache_mesh_vertex_colors_stamped.header.frame_id = cloud.header.frame_id;
    cache_mesh_vertex_colors_stamped.header.stamp = cloud.header.stamp;

    // Set uuid
    boost::uuids::uuid boost_uuid = boost::uuids::random_generator()();
    std::string uuid = boost::lexical_cast<std::string>(boost_uuid);
    cache_uuid = uuid;

    cache_mesh_geometry_stamped.uuid = uuid;
    cache_mesh_materials_stamped.uuid = uuid;
    cache_mesh_vertex_colors_stamped.uuid = uuid;

    return true;
}

bool Reconstruction::createMeshBufferFromPointBuffer(
    PointBufferPtr& point_buffer,
    lvr2::MeshBufferPtr<Vec>& mesh_buffer
)
{
    // Create a point cloud manager
    string pcm_name = config.pcm;
    lvr2::PointsetSurfacePtr<Vec> surface;

    // Create point set surface object
    if (pcm_name == "PCL")
    {
        lvr2::panic("PCL not supported right meow!");
    }
    else if (
        pcm_name == "STANN" ||
        pcm_name == "FLANN" ||
        pcm_name == "NABO" ||
        pcm_name == "NANOFLANN"
        )
    {
        surface = make_shared < lvr2::AdaptiveKSearchSurface < Vec >> (
            point_buffer,
            pcm_name,
            config.kn,
            config.ki,
            config.kd,
            config.ransac
        );
    }
    else
    {
        ROS_ERROR_STREAM("Unable to create PointCloudManager.");
        ROS_ERROR_STREAM("Unknown option '" << pcm_name << "'.");
        ROS_ERROR_STREAM("Available PCMs are: ");
        ROS_ERROR_STREAM("STANN, STANN_RANSAC, PCL");
        return 0;
    }

    // Set search config for normal estimation and distance evaluation
    surface->setKd(config.kd);
    surface->setKi(config.ki);
    surface->setKn(config.kn);

    // Calculate normals if necessary
    if (!point_buffer->hasNormals() || config.recalcNormals)
    {
        surface->calculateSurfaceNormals();
    }
    else
    {
        ROS_INFO_STREAM("Using given normals.");
    }

    // Create an empty mesh
    lvr2::HalfEdgeMesh <Vec> mesh;

    // Determine whether to use intersections or voxelsize
    float resolution;
    bool useVoxelsize;
    if (config.intersections > 0)
    {
        resolution = config.intersections;
        useVoxelsize = false;
    }
    else
    {
        resolution = config.voxelsize;
        useVoxelsize = true;
    }

    // Create a point set grid for reconstruction
    string decomposition = config.decomposition;

    // Fail safe check
    if (decomposition != "MC" && decomposition != "PMC" && decomposition != "SF")
    {
        ROS_ERROR_STREAM("Unsupported decomposition type " << decomposition << ". Defaulting to PMC.");
        decomposition = "PMC";
    }

    shared_ptr <lvr2::GridBase> grid;
    unique_ptr <lvr2::FastReconstructionBase<Vec>> reconstruction;
    if (decomposition == "MC")
    {
        lvr2::panic("MC decomposition type not supported right now!");
    }
    else if (decomposition == "PMC")
    {
        lvr2::BilinearFastBox<Vec>::m_surface = surface;
        auto ps_grid = std::make_shared<lvr2::PointsetGrid<Vec, lvr2::BilinearFastBox<Vec>>>(
            resolution,
            surface,
            surface->getBoundingBox(),
            useVoxelsize,
            !config.noExtrusion
        );
        ps_grid->calcDistanceValues();
        grid = ps_grid;
        reconstruction = make_unique<lvr2::FastReconstruction<Vec, lvr2::BilinearFastBox<Vec>>>(ps_grid);
    }
    else if (decomposition == "SF")
    {
        lvr2::panic("SF decomposition type not supported right now!");
    }

    // Create mesh
    reconstruction->getMesh(mesh);


    // =======================================================================
    // Optimize and finalize mesh
    // =======================================================================
    if(config.danglingArtifacts != 0)
    {
        removeDanglingCluster(mesh, static_cast<size_t>(config.danglingArtifacts));
    }

    // Magic number from lvr1 `cleanContours`...
    cleanContours(mesh, config.cleanContours, 0.0001);

    naiveFillSmallHoles(mesh, static_cast<size_t>(config.fillHoles), false);

    auto faceNormals = calcFaceNormals(mesh);

    lvr2::ClusterBiMap <lvr2::FaceHandle> clusterBiMap;
    if (config.optimizePlanes)
    {
        clusterBiMap = iterativePlanarClusterGrowing(
            mesh,
            faceNormals,
            config.normalThreshold,
            config.planeIterations,
            config.minPlaneSize
        );

        if (config.smallRegionThreshold > 0)
        {
            deleteSmallPlanarCluster(
                mesh,
                clusterBiMap,
                static_cast<size_t>(config.smallRegionThreshold)
            );
        }
    }
    else
    {
        clusterBiMap = planarClusterGrowing(mesh, faceNormals, config.normalThreshold);
    }

    // Calc normals for vertices
    auto vertexNormals = calcVertexNormals(mesh, faceNormals, *surface);

    // Prepare color data for finalizing
    auto vertexColors = calcColorFromPointCloud(mesh, surface);

    // Prepare finalize algorithm
    lvr2::ClusterFlatteningFinalizer<Vec> finalize(clusterBiMap);
    finalize.setVertexNormals(vertexNormals);
    if (vertexColors)
    {
        finalize.setVertexColors(*vertexColors);
    }

    // Materializer for face materials (colors and/or textures)
    lvr2::Materializer<Vec> materializer(
        mesh,
        clusterBiMap,
        faceNormals,
        *surface
    );

    // When using textures ...
    if (config.generateTextures)
    {
        // Set texturizer
        lvr2::Texturizer<Vec> texturizer(
            config.texelSize,
            config.texMinClusterSize,
            config.texMaxClusterSize
        );
        materializer.setTexturizer(texturizer);
    }
    // Generate materials
    lvr2::MaterializerResult<Vec> matResult = materializer.generateMaterials();
    // Add data to finalize algorithm
    finalize.setMaterializerResult(matResult);

    // Apply finalize algorithm
    // FinalizeAlgorithm will generate a lvr2::MeshBuffer, which for now will be converted back to old lvr::MeshBuffer
    // mesh_buffer = (*finalize.apply(mesh).get()).toOldBuffer();

    // Apply finalize algorithm
    // Do some weird stuff to convert boost:shared_ptr to std::shared_ptr
    boost::shared_ptr<lvr2::MeshBuffer<Vec>> meshBufferBoostPtr = finalize.apply(mesh);
    lvr2::MeshBuffer<Vec> bufferCopy = *meshBufferBoostPtr.get();
    mesh_buffer = make_shared<lvr2::MeshBuffer<Vec>>(bufferCopy);

    ROS_INFO_STREAM("Reconstruction finished!");
    return true;
}

/**********************************************************************************************************************/
// Utility & Main

float *Reconstruction::getStatsCoeffs(std::string filename) const
{
    float *result = new float[14];
    std::ifstream in(filename.c_str());
    if (in.good())
    {
        for (int i = 0; i < 14; i++)
        {
            in >> result[i];
        }
        in.close();
    }
    else
    {
        for (int i = 0; i < 14; i++)
        {
            result[i] = 0.5;
        }
    }
    return result;
}


} // namespace lvr_ros


int main(int argc, char **args)
{
    ros::init(argc, args, "reconstruction");
    lvr_ros::Reconstruction reconstruction;
    ros::spin();

    return 0;
}
