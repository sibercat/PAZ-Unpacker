#include "Tree.h"
#include <cassert>

namespace kukdh1 {
  bool TreeCompare(const std::unique_ptr<Tree> &a, const std::unique_ptr<Tree> &b) {
    return a->GetName().compare(b->GetName()) <= 0;
  }

  Tree::Tree(TREE_TYPE type) {
    pParent = nullptr;
    ttType = type;
    hThis = nullptr;
    liCapacity.QuadPart = 0;
    bAdded = FALSE;
  }

  Tree::~Tree() {
    // unique_ptr vectors clean up children automatically
  }

  void Tree::AddToTree(HWND hTree) {
    if (bAdded) {
      return;
    }

    if (ttType == TREE_TYPE_ROOT) {
      wchar_t rootLabel[] = L"/";
      hThis = AddTreeItem(hTree, nullptr, TVI_ROOT, rootLabel, (LPARAM)this);
    }
    else if (pParent != nullptr) {
      std::wstring temp;
      ConvertWidechar(sName, temp);
      hThis = AddTreeItem(hTree, pParent->GetHandle(), TVI_LAST, (WCHAR *)temp.c_str(), (LPARAM)this);
    }

    bAdded = TRUE;
  }

  void Tree::AddChildsToTree(HWND hTree) {
    if (ttType != TREE_TYPE_FILE) {
      for (auto &child : vChildFolders) {
        child->AddToTree(hTree);
      }
      for (auto &child : vChildFiles) {
        child->AddToTree(hTree);
      }
    }
  }

  void Tree::AddGrandchildsToTree(HWND hTree, LPVOID arg, std::function<void(LPVOID, size_t, size_t)> callback) {
    if (ttType != TREE_TYPE_FILE) {
      size_t stAll = 0;
      size_t i = 0;

      for (auto &child : vChildFolders) {
        stAll += child->GetFileCount() + child->GetFolderCount();
      }

      callback(arg, i, stAll);

      for (auto &child : vChildFolders) {
        child->AddChildsToTree(hTree);
        i += child->GetFileCount() + child->GetFolderCount();
        callback(arg, i, stAll);
      }
    }
  }

  void Tree::SetFileInfo(Tree* parent, std::string &name, FileInfo &fiFile) {
    assert(parent != nullptr);
    assert(!name.empty());
    pParent = parent;
    fiFileInfo = fiFile;
    sName = name;
    ttType = TREE_TYPE_FILE;
  }

  void Tree::SetFolderInfo(Tree* parent, std::string &name) {
    assert(parent != nullptr);
    assert(!name.empty());
    pParent = parent;
    sName = name;
    ttType = TREE_TYPE_FOLDER;
  }

  Tree* Tree::AddChild(std::unique_ptr<Tree> pChild) {
    assert(pChild != nullptr);
    Tree *raw = pChild.get();
    if (pChild->GetType() == TREE_TYPE_FILE) {
      vChildFiles.push_back(std::move(pChild));
    }
    else {
      vChildFolders.push_back(std::move(pChild));
    }
    return raw;
  }

  void Tree::SortChild() {
    if (ttType != TREE_TYPE_FILE) {
      for (auto &child : vChildFolders) {
        child->SortChild();
      }
      std::sort(vChildFolders.begin(), vChildFolders.end(), TreeCompare);
      std::sort(vChildFiles.begin(), vChildFiles.end(), TreeCompare);
    }
  }

  LARGE_INTEGER Tree::UpdateCapacity() {
    liCapacity.QuadPart = 0;

    if (ttType == TREE_TYPE_FILE) {
      liCapacity.QuadPart = fiFileInfo.uiOriginalSize;
    }
    else {
      for (auto &child : vChildFiles) {
        liCapacity.QuadPart += child->UpdateCapacity().QuadPart;
      }
      for (auto &child : vChildFolders) {
        liCapacity.QuadPart += child->UpdateCapacity().QuadPart;
      }
    }

    return liCapacity;
  }

  Tree* Tree::GetChildFolderWithName(std::string name) {
    assert(!name.empty());
    for (auto &child : vChildFolders) {
      if (child->GetName().compare(name) == 0) {
        return child.get();
      }
    }
    return nullptr;
  }

  Tree::TREE_TYPE Tree::GetType() const {
    return ttType;
  }

  const FileInfo& Tree::GetFileInfo() const {
    return fiFileInfo;
  }

  HTREEITEM Tree::GetHandle() const {
    return hThis;
  }

  BOOL Tree::IsAdded() const {
    return bAdded;
  }

  BOOL Tree::IsChildAdded() const {
    if (ttType != TREE_TYPE_FILE) {
      if (!vChildFiles.empty()) {
        return vChildFiles.front()->IsAdded();
      }
      else if (!vChildFolders.empty()) {
        return vChildFolders.front()->IsAdded();
      }
    }
    return TRUE;
  }

  BOOL Tree::IsGrandchildAdded() const {
    if (ttType != TREE_TYPE_FILE) {
      if (!vChildFolders.empty()) {
        return vChildFolders.front()->IsChildAdded();
      }
    }
    return TRUE;
  }

  const std::string& Tree::GetName() const {
    return sName;
  }

  LARGE_INTEGER Tree::GetCapacity() const {
    return liCapacity;
  }

  size_t Tree::GetFileCount() {
    return vChildFiles.size();
  }

  size_t Tree::GetFolderCount() {
    return vChildFolders.size();
  }

  void Tree::GetFileNodeList(std::vector<Tree*> &vList) {
    if (ttType == TREE_TYPE_FILE) {
      vList.push_back(this);
    }
    else {
      for (auto &child : vChildFolders) child->GetFileNodeList(vList);
      for (auto &child : vChildFiles)   child->GetFileNodeList(vList);
    }
  }

  void Tree::ResetAdded() {
    bAdded = FALSE;
    hThis  = nullptr;
    for (auto &child : vChildFolders) child->ResetAdded();
    for (auto &child : vChildFiles)   child->ResetAdded();
  }

  void Tree::GetFileList(std::vector<kukdh1::FileInfo> &vList) {
    if (ttType == TREE_TYPE_FILE) {
      vList.push_back(fiFileInfo);
    }
    else {
      for (auto &child : vChildFolders) {
        child->GetFileList(vList);
      }
      for (auto &child : vChildFiles) {
        child->GetFileList(vList);
      }
    }
  }
}
