//
// Copyright (C) 2019-2022 Pablo Delgado Krämer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "RenderPass.h"
#include "RenderBuffer.h"
#include "RenderParam.h"
#include "Mesh.h"
#include "Instancer.h"
#include "Material.h"
#include "Tokens.h"
#include "MaterialNetworkCompiler.h"

#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/camera.h>

#include <gi.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace
{
  std::string _MakeMaterialXColorMaterialSrc(const GfVec3f& color, const char* name)
  {
    // Prefer UsdPreviewSurface over MDL diffuse or unlit because we want to give a good first
    // impression (many people will try Pixar's Kitchen scene first), regardless of whether the user
    // is aware of the use or purpose of the displayColor attribute (as opposed to a preview material).
    static const char* USDPREVIEWSURFACE_MTLX_DOC = R"(
      <?xml version="1.0"?>
      <materialx version="1.38">
        <UsdPreviewSurface name="gatling_SR_%s" type="surfaceshader">
          <input name="diffuseColor" type="color3" value="%f, %f, %f" />
        </UsdPreviewSurface>
        <surfacematerial name="gatling_MAT_%s" type="material">
          <input name="surfaceshader" type="surfaceshader" nodename="gatling_SR_%s" />
        </surfacematerial>
      </materialx>
    )";

    return TfStringPrintf(USDPREVIEWSURFACE_MTLX_DOC, name, color[0], color[1], color[2], name, name);
  }

  float _CalculateBitangentSign(const GfVec3f& n, const GfVec3f& t, const GfVec3f& b)
  {
    return (GfDot(GfCross(t, b), n) > 0.0f) ? 1.0f : -1.0f;
  }

  // Based on the algorithm proposed by Eric Lengyel in FGED 2 (Listing 7.4)
  // http://foundationsofgameenginedev.com/FGED2-sample.pdf
  void _CalculateTextureTangents(const VtVec3iArray& meshFaces,
                                 const VtVec3fArray& meshPoints,
                                 const HdGatlingMesh::VertexAttr<GfVec3f>& meshNormals,
                                 const HdGatlingMesh::VertexAttr<GfVec2f>& meshTexCoords,
                                 VtVec3fArray& meshTangents,
                                 VtFloatArray& meshBitangentSigns)
  {
    const float EPS = 0.0001f;
    size_t tangentCount = meshNormals.array.size();

    VtVec3fArray tangents(tangentCount, GfVec3f(0.0f));
    VtVec3fArray bitangents(tangentCount, GfVec3f(0.0f));
    VtVec3fArray normals(tangentCount, GfVec3f(0.0f));

    for (int i = 0; i < meshFaces.size(); i++)
    {
      const auto& f = meshFaces[i];

      const auto& p0 = meshPoints[f[0]];
      const auto& p1 = meshPoints[f[1]];
      const auto& p2 = meshPoints[f[2]];
      const auto& n0 = meshNormals.array[meshNormals.indexed ? f[0] : (i * 3 + 0)];
      const auto& n1 = meshNormals.array[meshNormals.indexed ? f[1] : (i * 3 + 1)];
      const auto& n2 = meshNormals.array[meshNormals.indexed ? f[2] : (i * 3 + 2)];
      const auto& t0 = meshTexCoords.array[meshTexCoords.indexed ? f[0] : (i * 3 + 0)];
      const auto& t1 = meshTexCoords.array[meshTexCoords.indexed ? f[1] : (i * 3 + 1)];
      const auto& t2 = meshTexCoords.array[meshTexCoords.indexed ? f[2] : (i * 3 + 2)];

      GfVec3f e1 = p1 - p0, e2 = p2 - p0;
      float x1 = t1[0] - t0[0], x2 = t2[0] - t0[0];
      float y1 = t1[1] - t0[1], y2 = t2[1] - t0[1];

      GfVec3f t, b;
      float denom = x1 * y2 - x2 * y1;

      // The original algorithm does not handle this special case, causing NaNs!
      if (fabsf(denom) > EPS)
      {
        float r = (1.0f / denom);

        t = (e1 * y2 - e2 * y1) * r;
        b = (e2 * x1 - e1 * x2) * r;
      }
      else
      {
        // Fall back to default UV direction
        t = GfVec3f::YAxis();
        b = GfVec3f::XAxis();
      }

      int outIndex0 = meshNormals.indexed ? f[0] : (i * 3 + 0);
      int outIndex1 = meshNormals.indexed ? f[1] : (i * 3 + 1);
      int outIndex2 = meshNormals.indexed ? f[2] : (i * 3 + 2);

      // Assets can author out-of-range indices (f.i. Intel's Sponza scene). Skip those.
      if (outIndex0 >= tangentCount || outIndex1 >= tangentCount || outIndex2 >= tangentCount)
      {
        TF_WARN("invalid primvar index; skipping");
        continue;
      }

      normals[outIndex0] += n0;
      normals[outIndex1] += n1;
      normals[outIndex2] += n2;
      tangents[outIndex0] += t;
      tangents[outIndex1] += t;
      tangents[outIndex2] += t;
      bitangents[outIndex0] += b;
      bitangents[outIndex1] += b;
      bitangents[outIndex2] += b;
    }

    meshTangents.resize(tangentCount);
    meshBitangentSigns.resize(tangentCount);

    for (int i = 0; i < tangentCount; i++)
    {
      const GfVec3f& n = meshNormals.array[i].GetNormalized();

      // Robust special-case handling based on the logic from DirectXMesh:
      // https://github.com/microsoft/DirectXMesh/blob/5647700332a2a2504000529902ac3164c058d616/DirectXMesh/DirectXMeshTangentFrame.cpp#L126-L162

      GfVec3f t = tangents[i];
      t = (t - n * GfDot(n, t)); // Gram-Schmidt re-orthogonalization

      GfVec3f b = bitangents[i];
      b = (b - n * GfDot(n, b)) - (t * GfDot(t, b));

      float tLen = t.GetLength();
      float bLen = b.GetLength();

      if (tLen > 0.0f)
      {
        t = t.GetNormalized();
      }
      if (bLen > 0.0f)
      {
        b = b.GetNormalized();
      }

      if (tLen <= EPS || bLen <= EPS)
      {
        if (tLen > 0.5f)
        {
          b = GfCross(n, t);
        }
        else if (bLen > 0.5f)
        {
          t = GfCross(b, n);
        }
        else
        {
          float d0 = abs(n[0]);
          float d1 = abs(n[1]);
          float d2 = abs(n[2]);

          GfVec3f axis;
          if (d0 < d1)
          {
            axis = (d0 < d2) ? GfVec3f::XAxis() : GfVec3f::ZAxis();
          }
          else if (d1 < d2)
          {
            axis = GfVec3f::YAxis();
          }
          else
          {
            axis = GfVec3f::ZAxis();
          }

          t = GfCross(n, axis);
          b = GfCross(n, t);
        }
      }

      meshTangents[i] = t;
      meshBitangentSigns[i] = _CalculateBitangentSign(n, t, b);
    }
  }

  // Duff et al. 2017. Building an Orthonormal Basis, Revisited. JCGT.
  // Licensed under CC BY-ND 3.0: https://creativecommons.org/licenses/by-nd/3.0/
  void _DuffOrthonormalBasis(const GfVec3f& n, GfVec3f& tangent, GfVec3f& bitangent)
  {
    float nSign = (n[2] >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (nSign + n[2]);
    float b = n[0] * n[1] * a;
    tangent = GfVec3f(1.0f + nSign * n[0] * n[0] * a, nSign * b, -nSign * n[0]);
    bitangent = GfVec3f(b, nSign + n[1] * n[1] * a, -n[1]);
  }

  void _CalculateFallbackTangents(const VtVec3iArray& meshFaces,
                                  const HdGatlingMesh::VertexAttr<GfVec3f>& meshNormals,
                                  VtVec3fArray& meshTangents,
                                  VtFloatArray& meshBitangentSigns)
  {
    size_t normalCount = meshNormals.array.size();

    meshTangents.resize(normalCount);
    meshBitangentSigns.resize(normalCount);

    for (int i = 0; i < normalCount; i++)
    {
      const GfVec3f normal = meshNormals.array[i];

      GfVec3f tangent, bitangent;
      _DuffOrthonormalBasis(normal, tangent, bitangent);

      meshTangents[i] = tangent;
      meshBitangentSigns[i] = _CalculateBitangentSign(normal, tangent, bitangent);
    }
  }

  void _CalculateTangents(const VtVec3iArray& meshFaces,
                          const VtVec3fArray& meshPoints,
                          const HdGatlingMesh::VertexAttr<GfVec3f>& meshNormals,
                          const HdGatlingMesh::VertexAttr<GfVec2f>& meshTexCoords,
                          HdGatlingMesh::VertexAttr<GfVec3f>& meshTangents,
                          HdGatlingMesh::VertexAttr<float>& meshBitangentSigns)
  {
    bool hasTexCoords = meshTexCoords.array.size() > 0;

    if (hasTexCoords)
    {
      _CalculateTextureTangents(meshFaces, meshPoints, meshNormals, meshTexCoords, meshTangents.array, meshBitangentSigns.array);
    }
    else
    {
      _CalculateFallbackTangents(meshFaces, meshNormals, meshTangents.array, meshBitangentSigns.array);
    }

    meshTangents.indexed = meshNormals.indexed;
    meshBitangentSigns.indexed = meshNormals.indexed;
  }
}

HdGatlingRenderPass::HdGatlingRenderPass(HdRenderIndex* index,
                                         const HdRprimCollection& collection,
                                         const HdRenderSettingsMap& settings,
                                         const MaterialNetworkCompiler& materialNetworkCompiler,
                                         GiScene* scene)
  : HdRenderPass(index, collection)
  , m_scene(scene)
  , m_settings(settings)
  , m_materialNetworkCompiler(materialNetworkCompiler)
  , m_isConverged(false)
  , m_lastSceneStateVersion(UINT32_MAX)
  , m_lastSprimIndexVersion(UINT32_MAX)
  , m_lastRenderSettingsVersion(UINT32_MAX)
  , m_lastVisChangeCount(UINT32_MAX)
  , m_geomCache(nullptr)
  , m_shaderCache(nullptr)
{
  auto defaultDiffuseColor = GfVec3f(0.18f); // UsdPreviewSurface spec
  std::string defaultMatSrc = _MakeMaterialXColorMaterialSrc(defaultDiffuseColor, "invalid");

  m_defaultMaterial = giCreateMaterialFromMtlxStr(defaultMatSrc.c_str());
  TF_AXIOM(m_defaultMaterial);
}

void HdGatlingRenderPass::_ClearMaterials()
{
  for (GiMaterial* mat : m_materials)
  {
    giDestroyMaterial(mat);
  }
  m_materials.clear();
}

HdGatlingRenderPass::~HdGatlingRenderPass()
{
  if (m_geomCache)
  {
    giDestroyGeomCache(m_geomCache);
  }
  if (m_shaderCache)
  {
    giDestroyShaderCache(m_shaderCache);
  }

  giDestroyMaterial(m_defaultMaterial);
  _ClearMaterials();
}

bool HdGatlingRenderPass::IsConverged() const
{
  return m_isConverged;
}

GiVertex _MakeGiVertex(GfMatrix4d transform, GfMatrix4d normalMatrix, const GfVec3f& point, const GfVec3f& normal,
                       const GfVec2f& texCoords, const GfVec3f& tangent, float bitangentSign)
{
  GfVec3f newPoint = transform.Transform(point);

  GfVec3f newNormal = normalMatrix.TransformDir(normal);
  newNormal.Normalize();

  GfVec3f newTangent = transform.Transform(tangent);
  newTangent.Normalize();

  GiVertex vertex;
  vertex.pos[0] = newPoint[0];
  vertex.pos[1] = newPoint[1];
  vertex.pos[2] = newPoint[2];
  vertex.norm[0] = newNormal[0];
  vertex.norm[1] = newNormal[1];
  vertex.norm[2] = newNormal[2];
  vertex.u = texCoords[0];
  vertex.v = 1.0f - texCoords[1];
  vertex.tangent[0] = newTangent[0];
  vertex.tangent[1] = newTangent[1];
  vertex.tangent[2] = newTangent[2];
  vertex.bitangentSign = bitangentSign;

  return vertex;
}

void HdGatlingRenderPass::_BakeMeshGeometry(const HdGatlingMesh* mesh,
                                            GfMatrix4d transform,
                                            uint32_t materialIndex,
                                            std::vector<GiFace>& faces,
                                            std::vector<GiVertex>& vertices) const
{
  GfMatrix4d normalMatrix = transform.GetInverse().GetTranspose();

  const VtVec3iArray& meshFaces = mesh->GetFaces();
  const VtVec3fArray& meshPoints = mesh->GetPoints();
  const auto& meshNormals = mesh->GetNormals();
  const auto& meshTexCoords = mesh->GetTexCoords();
  auto meshTangents = mesh->GetTangents();
  auto meshBitangentSigns = mesh->GetBitangentSigns();

  bool hasTexCoords = meshTexCoords.array.size() > 0;
  bool calcTangents = meshTangents.array.empty();
  bool calcBitangentSigns = meshBitangentSigns.array.empty();

  if (!calcTangents && calcBitangentSigns)
  {
#if 0
    // If no bitangent signs have been found, chances are high that none have been authored in the first place.
    // Handedness may then be assumed to be positive, although force calculating the tangents could yield better results.
    calcTangents = true;
#else
    TF_WARN("tangents have been provided without handedness; assuming positive");
    size_t signCount = std::max(meshNormals.array.size(), meshTangents.array.size());
    meshBitangentSigns.array = VtFloatArray(signCount, 1.0f);
    meshBitangentSigns.indexed = meshNormals.indexed && meshTangents.indexed;
#endif
  }
  if (calcTangents)
  {
    _CalculateTangents(meshFaces, meshPoints, meshNormals, meshTexCoords, meshTangents, meshBitangentSigns);
  }

  bool isAnyPrimvarNotIndexed = !meshNormals.indexed || !meshTexCoords.indexed || !meshTangents.indexed;
  uint32_t vertexOffset = vertices.size();

  for (size_t i = 0; i < meshFaces.size(); i++)
  {
    const GfVec3i& vertexIndices = meshFaces[i];

    GiFace face;
    face.v_i[0] = vertexOffset + (isAnyPrimvarNotIndexed ? (i * 3 + 0) : vertexIndices[0]);
    face.v_i[1] = vertexOffset + (isAnyPrimvarNotIndexed ? (i * 3 + 1) : vertexIndices[1]);
    face.v_i[2] = vertexOffset + (isAnyPrimvarNotIndexed ? (i * 3 + 2) : vertexIndices[2]);

    // We always need three unique vertices per face.
    if (isAnyPrimvarNotIndexed)
    {
      for (size_t j = 0; j < 3; j++)
      {
        const GfVec3f& point = meshPoints[vertexIndices[j]];
        const GfVec3f& normal = meshNormals.array[meshNormals.indexed ? vertexIndices[j] : (i * 3 + j)];
        GfVec2f texCoords = hasTexCoords ? meshTexCoords.array[meshTexCoords.indexed ? vertexIndices[j] : (i * 3 + j)] : GfVec2f();

        GfVec3f tangent = meshTangents.array[meshTangents.indexed ? vertexIndices[j] : (i * 3 + j)];
        float bitangentSign = meshBitangentSigns.array[meshBitangentSigns.indexed ? vertexIndices[j] : (i * 3 + j)];

        GiVertex vertex = _MakeGiVertex(transform, normalMatrix, point, normal, texCoords, tangent, bitangentSign);
        vertices.push_back(vertex);
      }
    }

    faces.push_back(face);
  }

  // Early-out if the vertices are not indexed.
  if (isAnyPrimvarNotIndexed)
  {
    return;
  }

  for (size_t j = 0; j < meshPoints.size(); j++)
  {
    const GfVec3f& point = meshPoints[j];
    const GfVec3f& normal = meshNormals.array[j];
    GfVec2f texCoords = hasTexCoords ? meshTexCoords.array[j] : GfVec2f();

    GfVec3f tangent = meshTangents.array[j];
    float bitangentSign = meshBitangentSigns.array[j];

    GiVertex vertex = _MakeGiVertex(transform, normalMatrix, point, normal, texCoords, tangent, bitangentSign);
    vertices.push_back(vertex);
  }
}

void HdGatlingRenderPass::_BakeMeshes(HdRenderIndex* renderIndex,
                                      GfMatrix4d rootTransform,
                                      std::vector<const GiMaterial*>& materials,
                                      std::vector<const GiMesh*>& meshes,
                                      std::vector<GiMeshInstance>& instances)
{
  _ClearMaterials();

  TfHashMap<std::string, uint32_t, TfHash> materialMap;
  materialMap[""] = 0;

  materials.push_back(m_defaultMaterial);

  for (const auto& rprimId : renderIndex->GetRprimIds())
  {
    const HdRprim* rprim = renderIndex->GetRprim(rprimId);

    const HdGatlingMesh* mesh = static_cast<const HdGatlingMesh*>(rprim);
    if (!mesh)
    {
      continue;
    }

    if (!mesh->IsVisible())
    {
      continue;
    }

    VtMatrix4dArray transforms;
    const SdfPath& instancerId = mesh->GetInstancerId();

    if (instancerId.IsEmpty())
    {
      transforms.resize(1);
      transforms[0] = GfMatrix4d(1.0);
    }
    else
    {
      HdInstancer* boxedInstancer = renderIndex->GetInstancer(instancerId);
      HdGatlingInstancer* instancer = static_cast<HdGatlingInstancer*>(boxedInstancer);

      const SdfPath& meshId = mesh->GetId();
      transforms = instancer->ComputeInstanceTransforms(meshId);
    }

    const SdfPath& materialId = mesh->GetMaterialId();
    std::string materialIdStr = materialId.GetAsString();

    uint32_t materialIndex = 0;
    if (!materialId.IsEmpty() && materialMap.find(materialIdStr) != materialMap.end())
    {
      materialIndex = materialMap[materialIdStr];
    }
    else
    {
      HdSprim* sprim = renderIndex->GetSprim(HdPrimTypeTokens->material, materialId);
      HdGatlingMaterial* material = static_cast<HdGatlingMaterial*>(sprim);

      GiMaterial* giMat = nullptr;
      if (material)
      {
        const HdMaterialNetwork2* network = material->GetNetwork();

        if (network)
        {
          giMat = m_materialNetworkCompiler.CompileNetwork(sprim->GetId(), *network);

          if (giMat)
          {
            m_materials.push_back(giMat);
          }
        }
      }

      if (!giMat && mesh->HasColor())
      {
        // Try to reuse color material by including the RGB value in the name
        const GfVec3f& color = mesh->GetColor();
        materialIdStr = TfStringPrintf("color_%f_%f_%f", color[0], color[1], color[2]);
        std::replace(materialIdStr.begin(), materialIdStr.end(), '.', '_'); // _1.9_ -> _1_9_

        if (materialMap.find(materialIdStr) != materialMap.end())
        {
          materialIndex = materialMap[materialIdStr];
        }
        else
        {
          std::string colorMatSrc = _MakeMaterialXColorMaterialSrc(color, materialIdStr.c_str());
          GiMaterial* giColorMat = giCreateMaterialFromMtlxStr(colorMatSrc.c_str());
          if (giColorMat)
          {
            m_materials.push_back(giColorMat);
            giMat = giColorMat;
          }
        }
      }

      if (giMat)
      {
        materialIndex = materials.size();
        materials.push_back(giMat);
        materialMap[materialIdStr] = materialIndex;
      }
    }

    std::vector<GiFace> faces;
    std::vector<GiVertex> vertices;
    _BakeMeshGeometry(mesh, GfMatrix4d(1.0), materialIndex, faces, vertices);

    GiMeshDesc desc = {0};
    desc.faceCount = faces.size();
    desc.faces = faces.data();
    desc.material = materials[materialIndex];
    desc.vertexCount = vertices.size();
    desc.vertices = vertices.data();

    GiMesh* giMesh = giCreateMesh(&desc);
    assert(giMesh);
    meshes.push_back(giMesh);

    const GfMatrix4d& prototypeTransform = mesh->GetPrototypeTransform();
    for (size_t i = 0; i < transforms.size(); i++)
    {
      GfMatrix4d T = prototypeTransform * transforms[i];

      float instanceTransform[3][4] = {
        (float) T[0][0], (float) T[1][0], (float) T[2][0], (float) T[3][0],
        (float) T[0][1], (float) T[1][1], (float) T[2][1], (float) T[3][1],
        (float) T[0][2], (float) T[1][2], (float) T[2][2], (float) T[3][2]
      };

      GiMeshInstance instance;
      instance.mesh = giMesh;
      memcpy(instance.transform, instanceTransform, sizeof(instanceTransform));
      instances.push_back(instance);
    }
  }
}

void HdGatlingRenderPass::_ConstructGiCamera(const HdCamera& camera, GiCameraDesc& giCamera, bool clippingEnabled) const
{
  // We transform the scene into camera space at the beginning, so for
  // subsequent camera transforms, we need to 'substract' the initial transform.
  GfMatrix4d absInvViewMatrix = camera.GetTransform();
  GfMatrix4d relViewMatrix = absInvViewMatrix * m_rootMatrix;

  GfVec3d position = relViewMatrix.Transform(GfVec3d(0.0, 0.0, 0.0));
  GfVec3d forward = relViewMatrix.TransformDir(GfVec3d(0.0, 0.0, -1.0));
  GfVec3d up = relViewMatrix.TransformDir(GfVec3d(0.0, 1.0, 0.0));

  forward.Normalize();
  up.Normalize();

  // See https://wiki.panotools.org/Field_of_View
  float aperture = camera.GetVerticalAperture() * GfCamera::APERTURE_UNIT;
  float focalLength = camera.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT;
  float vfov = 2.0f * std::atan(aperture / (2.0f * focalLength));

  bool focusOn = true;
#if PXR_VERSION >= 2311
  focusOn = camera.GetFocusOn();
#endif

  giCamera.position[0] = (float) position[0];
  giCamera.position[1] = (float) position[1];
  giCamera.position[2] = (float) position[2];
  giCamera.forward[0] = (float) forward[0];
  giCamera.forward[1] = (float) forward[1];
  giCamera.forward[2] = (float) forward[2];
  giCamera.up[0] = (float) up[0];
  giCamera.up[1] = (float) up[1];
  giCamera.up[2] = (float) up[2];
  giCamera.vfov = vfov;
  giCamera.fStop = float(focusOn) * camera.GetFStop();
  giCamera.focusDistance = camera.GetFocusDistance();
  giCamera.focalLength = focalLength;
  giCamera.clipStart = clippingEnabled ? camera.GetClippingRange().GetMin() : 0.0f;
  giCamera.clipEnd = clippingEnabled ? camera.GetClippingRange().GetMax() : FLT_MAX;
}

const std::unordered_map<TfToken, GiAovId, TfToken::HashFunctor> s_aovIdMappings {
  { HdAovTokens->color,                     GI_AOV_ID_COLOR              },
  { HdAovTokens->normal,                    GI_AOV_ID_NORMAL             },
#ifndef NDEBUG
  { HdGatlingAovTokens->debugNee,           GI_AOV_ID_DEBUG_NEE          },
  { HdGatlingAovTokens->debugBarycentrics,  GI_AOV_ID_DEBUG_BARYCENTRICS },
  { HdGatlingAovTokens->debugTexcoords,     GI_AOV_ID_DEBUG_TEXCOORDS    },
  { HdGatlingAovTokens->debugBounces,       GI_AOV_ID_DEBUG_BOUNCES      },
  { HdGatlingAovTokens->debugClockCycles,   GI_AOV_ID_DEBUG_CLOCK_CYCLES },
  { HdGatlingAovTokens->debugOpacity,       GI_AOV_ID_DEBUG_OPACITY      },
  { HdGatlingAovTokens->debugTangents,      GI_AOV_ID_DEBUG_TANGENTS     },
  { HdGatlingAovTokens->debugBitangents,    GI_AOV_ID_DEBUG_BITANGENTS   },
#endif
};

const HdRenderPassAovBinding* _FilterAovBinding(const HdRenderPassAovBindingVector& aovBindings)
{
  for (const HdRenderPassAovBinding& aovBinding : aovBindings)
  {
    bool aovSupported = s_aovIdMappings.count(aovBinding.aovName) > 0;

    if (aovSupported)
    {
      return &aovBinding;
    }

    HdGatlingRenderBuffer* renderBuffer = static_cast<HdGatlingRenderBuffer*>(aovBinding.renderBuffer);
    renderBuffer->SetConverged(true);
    continue;
  }

  return nullptr;
}

GiAovId _GetAovId(const TfToken& aovName)
{
  GiAovId id = GI_AOV_ID_COLOR;

  auto iter = s_aovIdMappings.find(aovName);

  if (iter != s_aovIdMappings.end())
  {
    id = iter->second;
  }
  else
  {
    TF_CODING_ERROR(TfStringPrintf("Invalid AOV id %s", aovName.GetText()));
  }

  return id;
}

void HdGatlingRenderPass::_Execute(const HdRenderPassStateSharedPtr& renderPassState,
                                   const TfTokenVector& renderTags)
{
  TF_UNUSED(renderTags);

  m_isConverged = false;

  const HdCamera* camera = renderPassState->GetCamera();
  if (!camera)
  {
    return;
  }

  const HdRenderPassAovBindingVector& aovBindings = renderPassState->GetAovBindings();
  if (aovBindings.empty())
  {
    return;
  }

  const HdRenderPassAovBinding* aovBinding = _FilterAovBinding(aovBindings);
  if (!aovBinding)
  {
    TF_RUNTIME_ERROR("AOV not supported");
    return;
  }

  HdGatlingRenderBuffer* renderBuffer = static_cast<HdGatlingRenderBuffer*>(aovBinding->renderBuffer);
  if (renderBuffer->GetFormat() != HdFormatFloat32Vec4)
  {
    TF_RUNTIME_ERROR("Unsupported render buffer format");
    return;
  }

  HdRenderIndex* renderIndex = GetRenderIndex();
  HdChangeTracker& changeTracker = renderIndex->GetChangeTracker();
  HdRenderDelegate* renderDelegate = renderIndex->GetRenderDelegate();
  HdGatlingRenderParam* renderParam = static_cast<HdGatlingRenderParam*>(renderDelegate->GetRenderParam());

  uint32_t sceneStateVersion = changeTracker.GetSceneStateVersion();
  uint32_t sprimIndexVersion = changeTracker.GetSprimIndexVersion();
  uint32_t visibilityChangeCount = changeTracker.GetVisibilityChangeCount();
  uint32_t renderSettingsStateVersion = renderDelegate->GetRenderSettingsVersion();
  GiAovId aovId = _GetAovId(aovBinding->aovName);

  bool sceneChanged = (sceneStateVersion != m_lastSceneStateVersion);
  bool renderSettingsChanged = (renderSettingsStateVersion != m_lastRenderSettingsVersion);
  bool visibilityChanged = (m_lastVisChangeCount != visibilityChangeCount);
  bool aovChanged = (aovId != m_lastAovId);

  if (sceneChanged || renderSettingsChanged || visibilityChanged || aovChanged)
  {
    giInvalidateFramebuffer();
  }

  m_lastSceneStateVersion = sceneStateVersion;
  m_lastSprimIndexVersion = sprimIndexVersion;
  m_lastRenderSettingsVersion = renderSettingsStateVersion;
  m_lastVisChangeCount = visibilityChangeCount;
  m_lastAovId = aovId;

  bool rebuildShaderCache = !m_shaderCache || aovChanged || giShaderCacheNeedsRebuild() || renderSettingsChanged;

  bool rebuildGeomCache = !m_geomCache || visibilityChanged;

  if (rebuildShaderCache || rebuildGeomCache)
  {
    // Transform scene into camera space to increase floating point precision.
    // FIXME: reintroduce and don't apply rotation
    // https://pharr.org/matt/blog/2018/03/02/rendering-in-camera-space
    //GfMatrix4d viewMatrix = camera->GetTransform().GetInverse();
    m_rootMatrix = GfMatrix4d(1.0);// viewMatrix;

    // FIXME: cache results for shader cache rebuild
    std::vector<const GiMaterial*> materials;
    std::vector<const GiMesh*> meshes;
    std::vector<GiMeshInstance> instances;
    _BakeMeshes(renderIndex, m_rootMatrix, materials, meshes, instances);

    if (rebuildShaderCache)
    {
      if (m_shaderCache)
      {
        giDestroyShaderCache(m_shaderCache);
      }

      auto domeLightCameraVisibilityValueIt = m_settings.find(HdRenderSettingsTokens->domeLightCameraVisibility);

      GiShaderCacheParams shaderParams;
      shaderParams.aovId = aovId;
      shaderParams.depthOfField = m_settings.find(HdGatlingSettingsTokens->depthOfField)->second.Get<bool>();
      shaderParams.domeLightCameraVisible = (domeLightCameraVisibilityValueIt == m_settings.end()) || domeLightCameraVisibilityValueIt->second.GetWithDefault<bool>(true);
      shaderParams.filterImportanceSampling = m_settings.find(HdGatlingSettingsTokens->filterImportanceSampling)->second.Get<bool>();
      shaderParams.materialCount = materials.size();
      shaderParams.materials = materials.data();
      shaderParams.nextEventEstimation = m_settings.find(HdGatlingSettingsTokens->nextEventEstimation)->second.Get<bool>();
      shaderParams.progressiveAccumulation = m_settings.find(HdGatlingSettingsTokens->progressiveAccumulation)->second.Get<bool>();
      shaderParams.scene = m_scene;

      m_shaderCache = giCreateShaderCache(&shaderParams);
      TF_VERIFY(m_shaderCache, "Unable to create shader cache");
    }

    if (m_shaderCache && (rebuildGeomCache || giGeomCacheNeedsRebuild()))
    {
      if (m_geomCache)
      {
        giDestroyGeomCache(m_geomCache);
      }

      GiGeomCacheParams geomParams;
      geomParams.meshInstanceCount = instances.size();
      geomParams.meshInstances = instances.data();
      geomParams.shaderCache = m_shaderCache;

      m_geomCache = giCreateGeomCache(&geomParams);
      TF_VERIFY(m_geomCache, "Unable to create geom cache");
    }
  }

  if (!m_geomCache || !m_shaderCache)
  {
    return;
  }

  GfVec4f backgroundColor = aovBinding->clearValue.GetWithDefault<GfVec4f>(GfVec4f(0.f));

  bool clippingEnabled = renderPassState->GetClippingEnabled() &&
                         m_settings.find(HdGatlingSettingsTokens->clippingPlanes)->second.Get<bool>();

  GiCameraDesc giCamera;
  _ConstructGiCamera(*camera, giCamera, clippingEnabled);

  GiRenderParams renderParams;
  renderParams.camera = &giCamera;
  renderParams.geomCache = m_geomCache;
  renderParams.shaderCache = m_shaderCache;
  renderParams.renderBuffer = renderBuffer->GetGiRenderBuffer();
  renderParams.maxBounces = VtValue::Cast<int>(m_settings.find(HdGatlingSettingsTokens->maxBounces)->second).Get<int>();
  renderParams.spp = VtValue::Cast<int>(m_settings.find(HdGatlingSettingsTokens->spp)->second).Get<int>();
  renderParams.rrBounceOffset = VtValue::Cast<int>(m_settings.find(HdGatlingSettingsTokens->rrBounceOffset)->second).Get<int>();
  renderParams.lightIntensityMultiplier = VtValue::Cast<float>(m_settings.find(HdGatlingSettingsTokens->lightIntensityMultiplier)->second).Get<float>();
  renderParams.rrInvMinTermProb = VtValue::Cast<float>(m_settings.find(HdGatlingSettingsTokens->rrInvMinTermProb)->second).Get<float>();
  renderParams.maxSampleValue = VtValue::Cast<float>(m_settings.find(HdGatlingSettingsTokens->maxSampleValue)->second).Get<float>();
  renderParams.domeLight = renderParam->ActiveDomeLight();
  renderParams.scene = m_scene;
  for (uint32_t i = 0; i < 4; i++)
  {
    renderParams.backgroundColor[i] = backgroundColor[i];
  }

  float* imgData = (float*) renderBuffer->Map();

  int32_t result = giRender(&renderParams, imgData);

  TF_VERIFY(result == GI_OK, "Unable to render scene.");

  renderBuffer->Unmap();

  m_isConverged = true;
}

PXR_NAMESPACE_CLOSE_SCOPE
