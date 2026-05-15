#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

namespace bustub
{

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id,
                          BufferPoolManager* buffer_pool_manager,
                          const KeyComparator& comparator, int leaf_max_size,
                          int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id)
{
  WritePageGuard guard = bpm_ -> FetchPageWrite(header_page_id_);
  // In the original bpt, I fetch the header page
  // thus there's at least one page now
  auto root_header_page = guard.template AsMut<BPlusTreeHeaderPage>();
  // reinterprete the data of the page into "HeaderPage"
  root_header_page -> root_page_id_ = INVALID_PAGE_ID;
  // set the root_id to INVALID
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const  ->  bool
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page -> root_page_id_ == INVALID_PAGE_ID;
  // Just check if the root_page_id is INVALID
  // usage to fetch a page:
  // fetch the page guard   ->   call the "As" function of the page guard
  // to reinterprete the data of the page as "BPlusTreePage"
  return is_empty;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
 /*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType& key,
                              std::vector<ValueType>* result, Transaction* txn)
    -> bool {
  if (IsEmpty()) return false;

  ReadPageGuard guard = bpm_->FetchPageRead(GetRootPageId());
  auto page = guard.template As<BPlusTreePage>();

  while (!page->IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(page);
    int slot = BinaryFind(internal, key);
    /* int slot = -1, size = internal->GetSize();
    for (int i = 1; i < size; ++i)
    {
      if (comparator_(key, internal->KeyAt(i)) < 0) 
      {
        slot = i - 1;
        break;
      }
    }
    if (slot == -1) slot = size - 1; */
    guard = bpm_->FetchPageRead(internal->ValueAt(slot)); // value at slot is the index of son
    page = guard.template As<BPlusTreePage>();
  }

  auto leaf = reinterpret_cast<const LeafPage*>(page);
  int slot = BinaryFind(leaf, key);
  /* int slot = -1;
  for (int i = 0; i < leaf->GetSize(); ++i)
  {
    if (comparator_(leaf->KeyAt(i), key) == 0)
    {
      slot = i;
      break;
    }
  } */
  if (slot == -1) return false;
  result->push_back(leaf->ValueAt(slot));
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
 /*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType& key, const ValueType& value,
                            Transaction* txn) -> bool {
  if (IsEmpty()) // create a root node, which is a leaf
  {
    page_id_t new_root_id;
    auto root_guard = bpm_->NewPageGuarded(&new_root_id);
    auto leaf = root_guard.template AsMut<LeafPage>();
    leaf->Init(leaf_max_size_);
    leaf->SetPageType(IndexPageType::LEAF_PAGE);
    leaf->SetKeyAt(0, key);
    leaf->SetValueAt(0, value);
    leaf->SetSize(1);

    WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_);
    auto header = header_guard.template AsMut<BPlusTreeHeaderPage>();
    header->root_page_id_ = new_root_id;
    return true;
  }

  std::deque<WritePageGuard> path;
  page_id_t cur_id = GetRootPageId();
  WritePageGuard cur_guard = bpm_->FetchPageWrite(cur_id);
  auto cur_page = cur_guard.template AsMut<BPlusTreePage>();

  while (!cur_page->IsLeafPage())
  {
    path.push_back(std::move(cur_guard));
    auto internal = reinterpret_cast<InternalPage*>(cur_page);
    int slot = BinaryFind(internal, key);
    /* int slot = 0;
    for (int i = 1; i < internal->GetSize(); ++i)
    {
      if (comparator_(key, internal->KeyAt(i)) < 0) break;
      slot = i;
    } */
    cur_id = internal->ValueAt(slot);
    cur_guard = bpm_->FetchPageWrite(cur_id);
    cur_page = cur_guard.template AsMut<BPlusTreePage>();
  }
  path.push_back(std::move(cur_guard));
  page_id_t leaf_id = path.back().PageId();
  auto leaf = reinterpret_cast<LeafPage*>(path.back().template AsMut<BPlusTreePage>());

  if (BinaryFind(leaf, key) != -1) return false; // guarantee the uniqueness
  /* for (int i = 0; i < leaf->GetSize(); ++i)
  {
    if (comparator_(leaf->KeyAt(i), key) == 0) return false;
  } */

  int old_sz = leaf->GetSize();
  int pos = 0;
  while (pos < old_sz && comparator_(leaf->KeyAt(pos), key) < 0) ++pos;

  if (old_sz < leaf->GetMaxSize()) // Case 1: Insert 7
  {
    for (int i = old_sz; i > pos; --i)
    {
      leaf->SetKeyAt(i, leaf->KeyAt(i - 1));
      leaf->SetValueAt(i, leaf->ValueAt(i - 1));
    }
    leaf->SetKeyAt(pos, key);
    leaf->SetValueAt(pos, value);
    leaf->SetSize(old_sz + 1);
    return true;
  }

  // split the leaf
  int total = old_sz + 1;
  std::vector<std::pair<KeyType, ValueType>> tmp(total);
  for (int i = 0; i < old_sz; ++i)
  {
    tmp[i] = {leaf->KeyAt(i), leaf->ValueAt(i)};
  }
  for (int i = old_sz; i > pos; --i) tmp[i] = tmp[i - 1];
  tmp[pos] = {key, value};

  int split = (total + 1) / 2;
  for (int i = 0; i < split; ++i)
  {
    leaf->SetKeyAt(i, tmp[i].first);
    leaf->SetValueAt(i, tmp[i].second);
    leaf->SetSize(i + 1);
  }
  page_id_t new_leaf_id;
  auto new_leaf_guard = bpm_->NewPageGuarded(&new_leaf_id);
  auto new_leaf = new_leaf_guard.template AsMut<LeafPage>();
  new_leaf->Init(leaf_max_size_);
  new_leaf->SetPageType(IndexPageType::LEAF_PAGE);
  for (int i = split; i < total; ++i)
  {
    new_leaf->SetKeyAt(i - split, tmp[i].first);
    new_leaf->SetValueAt(i - split, tmp[i].second);
    new_leaf->SetSize(i - split + 1);
  }
  // insert new leaf to the leaf list
  new_leaf->SetNextPageId(leaf->GetNextPageId());
  leaf->SetNextPageId(new_leaf_id);

  auto up_key = new_leaf->KeyAt(0);
  page_id_t up_child = new_leaf_id;
  page_id_t left_child = leaf_id;
  path.pop_back();

  // spread the split of node
  while (true)
  {
    if (path.empty()) // create a new root
    {
      page_id_t new_root_id;
      auto new_root_guard = bpm_->NewPageGuarded(&new_root_id);
      auto new_root = new_root_guard.template AsMut<InternalPage>();
      new_root->Init(internal_max_size_);
      new_root->SetPageType(IndexPageType::INTERNAL_PAGE);
      new_root->SetValueAt(0, left_child);
      new_root->SetKeyAt(1, up_key);
      new_root->SetValueAt(1, up_child);
      new_root->SetSize(2);
      WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_); // set new root
      auto header = header_guard.template AsMut<BPlusTreeHeaderPage>();
      header->root_page_id_ = new_root_id;
      return true;
    }

    WritePageGuard parent_guard = std::move(path.back());
    path.pop_back();
    auto parent = parent_guard.template AsMut<InternalPage>();
    int parent_sz = parent->GetSize();

    if (parent_sz < parent->GetMaxSize()) // insert directly in the parent node
    {
      int ins = 1;   // find key
      while (ins < parent_sz && comparator_(parent->KeyAt(ins), up_key) < 0) ++ins;
      for (int i = parent_sz; i > ins; --i)
      {
        parent->SetKeyAt(i, parent->KeyAt(i - 1));
        parent->SetValueAt(i, parent->ValueAt(i - 1));
      }
      parent->SetKeyAt(ins, up_key);
      parent->SetValueAt(ins, up_child);
      parent->SetSize(parent_sz + 1);
      return true;
    }

    // father should also be split
    int total_int = parent_sz + 1;
    std::vector<KeyType> tmp_k(total_int);
    std::vector<page_id_t> tmp_v(total_int);
    for (int i = 0; i < parent_sz; ++i)
    {
      tmp_k[i] = parent->KeyAt(i);
      tmp_v[i] = parent->ValueAt(i);
    }
    int ins = 1;
    while (ins < parent_sz && comparator_(tmp_k[ins], up_key) < 0) ++ins;
    for (int i = parent_sz; i > ins; --i) {
      tmp_k[i] = tmp_k[i - 1];
      tmp_v[i] = tmp_v[i - 1];
    }
    tmp_k[ins] = up_key;
    tmp_v[ins] = up_child;

    // upload tmp_k[split_idx]
    int split_idx = total_int / 2;
    KeyType new_up_key = tmp_k[split_idx];
    // left: [0, split_idx-1]
    for (int i = 0; i < split_idx; ++i)
    {
      parent->SetKeyAt(i, tmp_k[i]);
      parent->SetValueAt(i, tmp_v[i]);
      parent->SetSize(i + 1);
    }
    
    // right: [split_idx + 1, total_idx), with index 0 defaulted
    page_id_t new_parent_id;
    auto new_parent_guard = bpm_->NewPageGuarded(&new_parent_id);
    auto new_parent = new_parent_guard.template AsMut<InternalPage>();
    new_parent->Init(internal_max_size_);
    new_parent->SetPageType(IndexPageType::INTERNAL_PAGE);
    for (int i = split_idx + 1; i < total_int; ++i) {
      new_parent->SetKeyAt(i - split_idx, tmp_k[i]);
      new_parent->SetValueAt(i - split_idx, tmp_v[i]);
      new_parent->SetSize(i - split_idx + 1);
    }
    new_parent->SetValueAt(0, tmp_v[split_idx]);

    up_key = new_up_key;
    up_child = new_parent_id;
    left_child = parent_guard.PageId();
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
 /*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType& key, Transaction* txn) {
  if (IsEmpty()) return;// return immediately

  std::deque<WritePageGuard> path;
  page_id_t cur_id = GetRootPageId();
  WritePageGuard cur_guard = bpm_->FetchPageWrite(cur_id);
  auto cur_page = cur_guard.template AsMut<BPlusTreePage>();

  while (!cur_page->IsLeafPage())
  {
    path.push_back(std::move(cur_guard));
    auto internal = reinterpret_cast<InternalPage*>(cur_page);
    int slot = BinaryFind(internal, key);
    cur_id = internal->ValueAt(slot);
    cur_guard = bpm_->FetchPageWrite(cur_id);
    cur_page = cur_guard.template AsMut<BPlusTreePage>();
  }
  path.push_back(std::move(cur_guard));
  page_id_t leaf_id = path.back().PageId();
  auto leaf = reinterpret_cast<LeafPage*>(path.back().template AsMut<BPlusTreePage>());

  // int pos = BinaryFind(leaf, key);
  int pos = -1, leaf_sz = leaf->GetSize();
  for (int i = 0; i < leaf_sz; ++i)
  {
    if (comparator_(leaf->KeyAt(i), key) == 0)
    {
      pos = i;
      break;
    }
  }
  if (pos == -1) return; // no relevant key in B+ Tree

  for (int i = pos; i < leaf_sz - 1; ++i) // delete the key in leaf
  {
    leaf->SetKeyAt(i, leaf->KeyAt(i + 1));
    leaf->SetValueAt(i, leaf->ValueAt(i + 1));
  }
  leaf->SetSize(leaf_sz - 1);

  if (path.size() == 1) // root is leaf page
  {
    if (leaf->GetSize() == 0) // clear all the tree
    {
      WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_);
      auto header = header_guard.template AsMut<BPlusTreeHeaderPage>();
      header->root_page_id_ = INVALID_PAGE_ID;
      bpm_->DeletePage(leaf_id);
    }
    return;
  }

  if (leaf->GetSize() >= leaf->GetMinSize()) return; // no need to redistribute

  
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const LeafPage* leaf_page, const KeyType& key)
    -> int {
  int l = 0, r = leaf_page->GetSize() - 1;
  while (l <= r)
  {
    int mid = (l + r) >> 1;
    int cmp = comparator_(leaf_page->KeyAt(mid), key);
    if (cmp == 0) return mid;
    if (cmp < 0) l = mid + 1;
    else r = mid - 1;
  }
  return -1;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const InternalPage* internal_page,
                                const KeyType& key) -> int {
  int l = 1, r = internal_page->GetSize() - 1;
  int ans = 0;
  while (l <= r)
  {
    int mid = (l + r) >> 1;
    if (comparator_(internal_page->KeyAt(mid), key) <= 0)
    {
      ans = mid;
      l = mid + 1;
    }
    else r = mid - 1;
  }
  return ans;
}

/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin()  ->  INDEXITERATOR_TYPE
//Just go left forever
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);
  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    int slot_num = 0;
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  int slot_num = 0;
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), 0);
  }
  return End();
}


/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key)  ->  INDEXITERATOR_TYPE
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();
  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1)
    {
      return End();
    }
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), slot_num);
  }
  return End();
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End()  ->  INDEXITERATOR_TYPE
{
  return INDEXITERATOR_TYPE(bpm_, -1, -1);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId()  ->  page_id_t
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  page_id_t root_page_id = root_header_page -> root_page_id_;
  return root_page_id;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

/*
 * This method is used for test only
 * Read data from file and insert/remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BatchOpsFromFile(const std::string& file_name,
                                      Transaction* txn)
{
  int64_t key;
  char instruction;
  std::ifstream input(file_name);
  while (input)
  {
    input >> instruction >> key;
    RID rid(key);
    KeyType index_key;
    index_key.SetFromInteger(key);
    switch (instruction)
    {
      case 'i':
        Insert(index_key, rid, txn);
        break;
      case 'd':
        Remove(index_key, txn);
        break;
      default:
        break;
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* bpm)
{
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage* page)
{
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf -> GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      std::cout << leaf -> KeyAt(i);
      if ((i + 1) < leaf -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else
  {
    auto* internal = reinterpret_cast<const InternalPage*>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      std::cout << internal -> KeyAt(i) << ": " << internal -> ValueAt(i);
      if ((i + 1) < internal -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      auto guard = bpm_ -> FetchPageBasic(internal -> ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* bpm, const std::string& outf)
{
  if (IsEmpty())
  {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage* page,
                             std::ofstream& out)
{
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">"
        << "max_size=" << leaf -> GetMaxSize()
        << ",min_size=" << leaf -> GetMinSize() << ",size=" << leaf -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      out << "<TD>" << leaf -> KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf -> GetNextPageId() != INVALID_PAGE_ID)
    {
      out << leaf_prefix << page_id << "   ->   " << leaf_prefix
          << leaf -> GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix
          << leaf -> GetNextPageId() << "};\n";
    }
  }
  else
  {
    auto* inner = reinterpret_cast<const InternalPage*>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">"
        << "max_size=" << inner -> GetMaxSize()
        << ",min_size=" << inner -> GetMinSize() << ",size=" << inner -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      out << "<TD PORT=\"p" << inner -> ValueAt(i) << "\">";
      // if (i > 0) {
      out << inner -> KeyAt(i) << "  " << inner -> ValueAt(i);
      // } else {
      // out << inner  ->  ValueAt(0);
      // }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      auto child_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0)
      {
        auto sibling_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page -> IsLeafPage() && !child_page -> IsLeafPage())
        {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId()
              << " " << internal_prefix << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId()
          << "   ->   ";
      if (child_page -> IsLeafPage())
      {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      }
      else
      {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree()  ->  std::string
{
  if (IsEmpty())
  {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id)
     ->  PrintableBPlusTree
{
  auto root_page_guard = bpm_ -> FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page -> IsLeafPage())
  {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page -> ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page -> ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page -> GetSize(); i++)
  {
    page_id_t child_id = internal_page -> ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub