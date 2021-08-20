#pragma once

#include <pxr/imaging/hd/material.h>

namespace MaterialX
{
  using DocumentPtr = std::shared_ptr<class Document>;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdGatlingMaterial final : public HdMaterial
{
public:
  HdGatlingMaterial(const SdfPath& id);

  ~HdGatlingMaterial() override;

public:
  const char* GetMaterialXDocumentString() const;

public:
  void Sync(HdSceneDelegate* sceneDelegate,
            HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override;

  HdDirtyBits GetInitialDirtyBitsMask() const override;

private:
  bool _GetMaterialNetworkSurfaceTerminal(const HdMaterialNetwork2& network2, HdMaterialNode2& surfaceTerminal);
  void _LoadMaterialXStandardLibrary(MaterialX::DocumentPtr doc);
  void _CreateMaterialXDocumentFromMaterialNetwork2(const HdMaterialNetwork2& network, MaterialX::DocumentPtr& doc);
  void _ProcessMaterialNetwork2(const HdMaterialNetwork2& network);
  void _ProcessMaterialNetworkMap(const HdMaterialNetworkMap& networkMap);

private:
  std::string m_mtlxDocStr;
};

PXR_NAMESPACE_CLOSE_SCOPE
