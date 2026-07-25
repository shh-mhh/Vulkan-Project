#pragma once

#include <vk_types.h>
#include <unordered_map>
#include <filesystem>

struct GeoSurface {
    uint32_t startIndex;
    uint32_t count;
};

// each mesh will have a name, based on its file. 
// we have a vector of surfaces, where each "surface" is a submesh of the GLTF mesh.
// we create a GeoSurface struct for each surface, as for each drawcall, we will draw a submesh. for that, we need to index though each vertex in the submesh, and add their data to the mesh buffer 
// (so we can send it to the vertex shader). this is why GeoSurface has only a startIndex and count, so we can index through, and 'collect' the vertex data.
// each mesh will have it's own mesh buffer. 
struct MeshAsset {
    std::string name;

    std::vector<GeoSurface> surfaces;
    GPUMeshBuffers meshBuffers;
};

//forward declaration
class VulkanEngine;


std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(VulkanEngine* engine, std::filesystem::path filePath);