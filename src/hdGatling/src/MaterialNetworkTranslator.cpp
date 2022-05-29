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

#include "MaterialNetworkTranslator.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Library.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Definition.h>
#include <MaterialXFormat/File.h>
#include <MaterialXFormat/Util.h>

#include <gi.h>

#include "Tokens.h"

namespace mx = MaterialX;

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
  _tokens,
  // USD node types
  (UsdPreviewSurface)
  (UsdUVTexture)
  (UsdTransform2d)
  (UsdPrimvarReader_float)
  (UsdPrimvarReader_float2)
  (UsdPrimvarReader_float3)
  (UsdPrimvarReader_float4)
  (UsdPrimvarReader_int)
  (UsdPrimvarReader_string)
  (UsdPrimvarReader_normal)
  (UsdPrimvarReader_point)
  (UsdPrimvarReader_vector)
  (UsdPrimvarReader_matrix)
  // MaterialX USD node type equivalents
  (ND_UsdPreviewSurface_surfaceshader)
  (ND_UsdUVTexture)
  (ND_UsdPrimvarReader_integer)
  (ND_UsdPrimvarReader_boolean)
  (ND_UsdPrimvarReader_string)
  (ND_UsdPrimvarReader_float)
  (ND_UsdPrimvarReader_vector2)
  (ND_UsdPrimvarReader_vector3)
  (ND_UsdPrimvarReader_vector4)
  (ND_UsdTransform2d)
  (ND_UsdPrimvarReader_matrix44)
);

bool _ConvertNodesToMaterialXNodes(const HdMaterialNetwork2& network,
                                   HdMaterialNetwork2& mtlxNetwork)
{
  mtlxNetwork = network;

  for (auto nodeIt = mtlxNetwork.nodes.begin(); nodeIt != mtlxNetwork.nodes.end(); nodeIt++)
  {
    TfToken& nodeTypeId = nodeIt->second.nodeTypeId;

    SdrRegistry& sdrRegistry = SdrRegistry::GetInstance();
    if (sdrRegistry.GetShaderNodeByIdentifierAndType(nodeTypeId, HdGatlingDiscoveryTypes->mtlx))
    {
      continue;
    }

    if (nodeTypeId == _tokens->UsdPreviewSurface)
    {
      nodeTypeId = _tokens->ND_UsdPreviewSurface_surfaceshader;
    }
    else if (nodeTypeId == _tokens->UsdUVTexture)
    {
      nodeTypeId = _tokens->ND_UsdUVTexture;
    }
    else if (nodeTypeId == _tokens->UsdTransform2d)
    {
      nodeTypeId = _tokens->ND_UsdTransform2d;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_float)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_float;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_float2)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_vector2;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_float3)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_vector3;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_float4)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_vector4;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_int)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_integer;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_string)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_string;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_normal)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_vector3;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_point)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_vector3;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_vector)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_vector3;
    }
    else if (nodeTypeId == _tokens->UsdPrimvarReader_matrix)
    {
      nodeTypeId = _tokens->ND_UsdPrimvarReader_matrix44;
    }
    else
    {
      TF_WARN("Unable to translate material node of type %s to MaterialX counterpart", nodeTypeId.GetText());
      return false;
    }
  }

  return true;
}

bool _GetMaterialNetworkSurfaceTerminal(const HdMaterialNetwork2& network2, HdMaterialNode2& terminalNode, SdfPath& terminalPath)
{
  const auto& connectionIt = network2.terminals.find(HdMaterialTerminalTokens->surface);

  if (connectionIt == network2.terminals.end())
  {
    return false;
  }

  const HdMaterialConnection2& connection = connectionIt->second;

  terminalPath = connection.upstreamNode;

  const auto& nodeIt = network2.nodes.find(terminalPath);

  if (nodeIt == network2.nodes.end())
  {
    return false;
  }

  terminalNode = nodeIt->second;

  return true;
}

MaterialNetworkTranslator::MaterialNetworkTranslator(const std::string& mtlxLibPath)
{
  m_nodeLib = mx::createDocument();

  mx::FilePathVec libFolders; // All directories if left empty.
  mx::FileSearchPath folderSearchPath(mtlxLibPath);
  mx::loadLibraries(libFolders, folderSearchPath, m_nodeLib);
}

gi_material* MaterialNetworkTranslator::ParseNetwork(const SdfPath& id,
                                                     const HdMaterialNetwork2& network) const
{
  gi_material* result = TryParseMdlNetwork(network);

  if (!result)
  {
    result = TryParseMtlxNetwork(id, network);
  }

  return result;
}

gi_material* MaterialNetworkTranslator::TryParseMdlNetwork(const HdMaterialNetwork2& network) const
{
  if (network.nodes.size() != 1)
  {
    return nullptr;
  }

  const HdMaterialNode2& node = network.nodes.begin()->second;

  SdrRegistry& sdrRegistry = SdrRegistry::GetInstance();
  SdrShaderNodeConstPtr sdrNode = sdrRegistry.GetShaderNodeByIdentifier(node.nodeTypeId);

  if (!sdrNode || sdrNode->GetContext() != HdGatlingNodeContexts->mdl)
  {
    return nullptr;
  }

  const NdrTokenMap& metadata = sdrNode->GetMetadata();
  const auto& subIdentifierIt = metadata.find(HdGatlingNodeMetadata->subIdentifier);
  TF_DEV_AXIOM(subIdentifierIt != metadata.end());

  const std::string& subIdentifier = (*subIdentifierIt).second;
  const std::string& fileUri = sdrNode->GetResolvedImplementationURI();

  return giCreateMaterialFromMdlFile(fileUri.c_str(), subIdentifier.c_str());
}

gi_material* MaterialNetworkTranslator::TryParseMtlxNetwork(const SdfPath& id, const HdMaterialNetwork2& network) const
{
  HdMaterialNetwork2 mtlxNetwork;
  if (!_ConvertNodesToMaterialXNodes(network, mtlxNetwork))
  {
    return nullptr;
  }

  mx::DocumentPtr doc = CreateMaterialXDocumentFromNetwork(id, mtlxNetwork);
  if (!doc)
  {
    return nullptr;
  }

  mx::string docStr = mx::writeToXmlString(doc);

  return giCreateMaterialFromMtlx(docStr.c_str());
}

mx::DocumentPtr MaterialNetworkTranslator::CreateMaterialXDocumentFromNetwork(const SdfPath& id,
                                                                              const HdMaterialNetwork2& network) const
{
  HdMaterialNode2 terminalNode;
  SdfPath terminalPath;
  if (!_GetMaterialNetworkSurfaceTerminal(network, terminalNode, terminalPath))
  {
    TF_WARN("Unable to find surface terminal for material network");
    return nullptr;
  }

  std::set<SdfPath> hdTextureNodes;
  mx::StringMap mxHdTextureMap;

  return HdMtlxCreateMtlxDocumentFromHdNetwork(
    network,
    terminalNode,
    terminalPath,
    id,
    m_nodeLib,
    &hdTextureNodes,
    &mxHdTextureMap
  );
}

PXR_NAMESPACE_CLOSE_SCOPE
