#include "Material.h"

#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usdImaging/usdImaging/tokens.h>

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Library.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Definition.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>

PXR_NAMESPACE_OPEN_SCOPE

namespace mx = MaterialX;

HdGatlingMaterial::HdGatlingMaterial(const SdfPath& id)
  : HdMaterial(id)
{
}

HdGatlingMaterial::~HdGatlingMaterial()
{
}

const char* HdGatlingMaterial::GetMaterialXDocumentString() const
{
  return m_mtlxDocStr.c_str();
}

void HdGatlingMaterial::Sync(HdSceneDelegate* sceneDelegate,
                             HdRenderParam* renderParam,
                             HdDirtyBits* dirtyBits)
{
  TF_UNUSED(renderParam);

  bool pullMaterial = (*dirtyBits & DirtyBits::DirtyParams);

  *dirtyBits = DirtyBits::Clean;

  if (!pullMaterial)
  {
    return;
  }

  const SdfPath& id = GetId();

  const VtValue& resource = sceneDelegate->GetMaterialResource(id);

  if (!resource.IsHolding<HdMaterialNetworkMap>())
  {
    return;
  }

  const HdMaterialNetworkMap& networkMap = resource.UncheckedGet<HdMaterialNetworkMap>();

  _ProcessMaterialNetworkMap(networkMap);
}

HdDirtyBits HdGatlingMaterial::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyParams;
}

bool HdGatlingMaterial::_GetMaterialNetworkSurfaceTerminal(const HdMaterialNetwork2& network2, HdMaterialNode2& surfaceTerminal)
{
  const auto& surfaceTerminalConnectionIt = network2.terminals.find(HdMaterialTerminalTokens->surface);

  if (surfaceTerminalConnectionIt == network2.terminals.end())
  {
    return false;
  }

  const HdMaterialConnection2& surfaceTerminalConnection = surfaceTerminalConnectionIt->second;

  const SdfPath& surfaceNodePath = surfaceTerminalConnection.upstreamNode;

  const auto& surfaceNodeIt = network2.nodes.find(surfaceNodePath);

  if (surfaceNodeIt == network2.nodes.end())
  {
    return false;
  }

  surfaceTerminal = surfaceNodeIt->second;
  return true;
}

void HdGatlingMaterial::_LoadMaterialXStandardLibrary(mx::DocumentPtr doc)
{
  const auto librariesPath = mx::FilePath("C:/Users/pablode/tmp/BlenderUSDHydraAddon2/bin/MaterialX/install/libraries"); // TODO: dynamic path

  mx::FileSearchPath folderSearchPath;
  folderSearchPath.append(librariesPath);

  const std::unordered_set<std::string> folderNames{ "targets", "stdlib", "pbrlib", "bxdf", "lights" };

  mx::loadLibraries(
    mx::FilePathVec(folderNames.begin(), folderNames.end()),
    folderSearchPath,
    doc
  );
}

void HdGatlingMaterial::_CreateMaterialXDocumentFromMaterialNetwork2(const HdMaterialNetwork2& network, mx::DocumentPtr& doc)
{
  HdMaterialNode2 surfaceTerminal;
  if (!_GetMaterialNetworkSurfaceTerminal(network, surfaceTerminal))
  {
    TF_WARN("Unable to find surface terminal for material network");
    return;
  }

  mx::DocumentPtr mtlxStdLib = mx::createDocument(); // TODO: cache this statically
  _LoadMaterialXStandardLibrary(mtlxStdLib);

  const SdfPath& id = GetId();
  std::set<SdfPath> hdTextureNodes;
  MaterialX::StringMap mxHdTextureMap;

  doc = HdMtlxCreateMtlxDocumentFromHdNetwork(
    network,
    surfaceTerminal,
    id,
    // TODO: stdlib UsdPreviewSurface does not seem to be taken into account for translation.
    // maybe related to: https://github.com/PixarAnimationStudios/USD/issues/1586
    mtlxStdLib,
    &hdTextureNodes,
    &mxHdTextureMap
  );
}

void HdGatlingMaterial::_ProcessMaterialNetwork2(const HdMaterialNetwork2& network)
{
  mx::DocumentPtr doc;
  _CreateMaterialXDocumentFromMaterialNetwork2(network, doc);

  m_mtlxDocStr = mx::writeToXmlString(doc);
}

void HdGatlingMaterial::_ProcessMaterialNetworkMap(const HdMaterialNetworkMap& networkMap)
{
  // Convert legacy HdMaterialNetworkMap to HdMaterialNetwork2.
  bool isVolume = false;
  HdMaterialNetwork2 network2;
  HdMaterialNetwork2ConvertFromHdMaterialNetworkMap(networkMap, &network2, &isVolume);

  if (isVolume)
  {
    TF_WARN("Volumes not supported");
    return;
  }

  _ProcessMaterialNetwork2(network2);
}

PXR_NAMESPACE_CLOSE_SCOPE
