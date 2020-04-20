#include "offset_tree_cont.h"
#include "parse_args.h"  // setup_base()
#include "learner.h"     // init_learner()
#include <algorithm>
#include "reductions.h"
#include "debug_log.h"
#include <cassert>
#include "hash.h"
#include "explore.h"
#include "explore_internal.h"


using namespace VW::config;
using namespace LEARNER;

using CB::cb_class;
using std::vector;

VW_DEBUG_ENABLE(false)

namespace VW
{
namespace offset_tree_cont
{
tree_node::tree_node(
    uint32_t node_id, uint32_t left_node_id, uint32_t right_node_id, uint32_t p_id, uint32_t depth, 
    bool left_only, bool right_only, bool is_leaf)
    : id(node_id)
    , left_id(left_node_id)
    , right_id(right_node_id)
    , parent_id(p_id)
    , depth(depth)
    , left_only(left_only)
    , right_only(right_only)
    , is_leaf(is_leaf)
    , learn_count(0)
{
}

bool tree_node::operator==(const tree_node& rhs) const
{
  if (this == &rhs)
    return true;
  return (id == rhs.id && left_id == rhs.left_id && right_id == rhs.right_id && parent_id == rhs.parent_id && 
  depth == rhs.depth && left_only == rhs.left_only && right_only == rhs.right_only && is_leaf == rhs.is_leaf);
}

bool tree_node::operator!=(const tree_node& rhs) const { return !(*this == rhs); }

void min_depth_binary_tree::build_tree(uint32_t num_nodes, uint32_t bandwidth)
{
  // Sanity checks
  if (_initialized)
  {
    if (num_nodes != _num_leaf_nodes)
    {
      THROW("Tree already initialized.  New leaf node count (" << num_nodes << ") does not equal current value. ("
                                                               << _num_leaf_nodes << ")");
    }
    return;
  }

  _num_leaf_nodes = num_nodes;
  // deal with degenerate cases of 0 and 1 actions
  if (_num_leaf_nodes == 0)
  {
    _initialized = true;
    return;
  }

  try
  {
    // Number of nodes in a minimal binary tree := (2 * LeafCount) - 1
    nodes.reserve(2 * _num_leaf_nodes - 1);

    //  Insert Root Node: First node in the collection, Parent is itself
    //  {node_id, left_id, right_id, parent_id, depth, right_only, left_only, is_leaf}
    nodes.emplace_back(0, 0, 0, 0, 0, false, false, true);

    uint32_t depth = 0, depth_const = 1;
    for (uint32_t i = 0; i < _num_leaf_nodes - 1; ++i)
    {
      nodes[i].left_id = 2 * i + 1;
      nodes[i].right_id = 2 * i + 2;
      nodes[i].is_leaf = false;
      if (2 * i + 1 >= depth_const)
        depth_const = (1 << (++depth + 1)) - 1;
      
      uint32_t id = 2 * i + 1;
      bool right_only = false;
      bool left_only = false;
      if (bandwidth)
      {
        right_only = (id == (_num_leaf_nodes/(2*bandwidth) - 1));
        left_only = (id == (_num_leaf_nodes/(bandwidth) - 2));
      }
      nodes.emplace_back(id, 0, 0, i, depth, left_only, right_only, true);
      
      id = 2 * i + 2;
      if (bandwidth)
      {
        right_only = (id == (_num_leaf_nodes/(2*bandwidth) - 1));
        left_only = (id == (_num_leaf_nodes/(bandwidth) - 2));
      }
      nodes.emplace_back(id, 0, 0, i, depth, left_only, right_only, true);
    }

    _initialized = true;
    _depth = depth;
  }
  catch (std::bad_alloc& e)
  {
    THROW("Unable to allocate memory for offset tree.  Label count:" << _num_leaf_nodes << " bad_alloc:" << e.what());
  }
}

uint32_t min_depth_binary_tree::internal_node_count() const { return (uint32_t)nodes.size() - _num_leaf_nodes; }

uint32_t min_depth_binary_tree::leaf_node_count() const { return _num_leaf_nodes; }

uint32_t min_depth_binary_tree::depth() const { return _depth; }

const tree_node& min_depth_binary_tree::get_sibling(const tree_node& v)
{
  // We expect not to get called on root
  const tree_node& v_parent = nodes[v.parent_id];
  return nodes[(v.id == v_parent.left_id) ? v_parent.right_id : v_parent.left_id];
}

std::string min_depth_binary_tree::tree_stats_to_string()
{
  std::stringstream treestats;
  treestats << "Learn() count per node: ";
  for (const tree_node& n : nodes)
  {
    if (n.is_leaf || n.id >= 16)
      break;

    treestats << "id=" << n.id << ", #l=" << n.learn_count << "; ";
  }
  return treestats.str();
}

void offset_tree::init(uint32_t num_actions, uint32_t bandwidth) { _binary_tree.build_tree(num_actions, bandwidth); }

int32_t offset_tree::learner_count() const { return _binary_tree.internal_node_count(); }

uint32_t offset_tree::predict(LEARNER::single_learner& base, example& ec)
{
  const vector<tree_node>& nodes = _binary_tree.nodes;

  // Handle degenerate cases of zero node trees
  if (_binary_tree.leaf_node_count() == 0)  // todo: chnage this to throw error at some point
    return 0;
  const CB::label saved_label = ec.l.cb;
  ec.l.simple.label = FLT_MAX;  // says it is a test example
  auto cur_node = nodes[0];

  while (!(cur_node.is_leaf))
  {
    if (cur_node.right_only)
      cur_node = nodes[cur_node.right_id];
    else if (cur_node.left_only)
      cur_node = nodes[cur_node.left_id];
    else
    {
      ec.partial_prediction = 0.f;
      ec.pred.scalar = 0.f;
      ec.l.simple.initial = 0.f;  // needed for gd.predict()
      base.predict(ec, cur_node.id);
      VW_DBG(_dd) << "otree_c: predict() after base.predict() " << scalar_pred_to_string(ec)
                  << ", nodeid = " << cur_node.id << std::endl;
      if (ec.pred.scalar < 0)  // TODO: check
      {
        cur_node = nodes[cur_node.left_id];
      }
      else
      {
        cur_node = nodes[cur_node.right_id];
      } 
    }
  }
  ec.l.cb = saved_label;
  return (cur_node.id - _binary_tree.internal_node_count() + 1);  // 1 to k
}

void offset_tree::init_node_costs(v_array<cb_class>& ac)
{
  assert(ac.size() == 0); 
  assert(ac[0].action > 0);

  _cost_star = ac[0].cost / ac[0].probability;

  uint32_t node_id = ac[0].action + _binary_tree.internal_node_count() - 1;
    VW_DBG(_dd) << "otree_c: learn() ac[0].action  = " << ac[0].action << ", node_id  = " << node_id
                << std::endl;
  _a = {node_id, _cost_star};

  node_id = ac[ac.size()-1].action + _binary_tree.internal_node_count() - 1;
    VW_DBG(_dd) << "otree_c: learn() ac[1].action  = " << ac[ac.size()-1].action << ", node_id  = " << node_id
                << std::endl;
  _b = {node_id, _cost_star};
}

constexpr float RIGHT = 1.0f;
constexpr float LEFT = -1.0f;


float offset_tree::return_cost(const tree_node& w)
{
  if (w.id < _a.node_id)
    return 0;
  else if (w.id == _a.node_id)
    return _a.cost;
  else if (w.id < _b.node_id)
    return _cost_star;
  else if (w.id == _b.node_id)
    return _b.cost;
  else
    return 0;
}

void offset_tree::learn(LEARNER::single_learner& base, example& ec)
{
  const polylabel saved_label = ec.l;
  const float saved_weight = ec.weight;
  const polyprediction saved_pred = ec.pred;

  const vector<tree_node>& nodes = _binary_tree.nodes;
  v_array<cb_class>& ac = ec.l.cb.costs;

  // Store the reduction indent depth for debug logging
  // without having to carry example around
  _dd = ec.stack_depth;

  VW_DBG(_dd) << "otree_c: learn() -- tree_traversal -- " << std::endl;

  init_node_costs(ac);

  for (uint32_t d = _binary_tree.depth(); d > 0; d--)
  {
    std::vector<node_cost> set_d = {_a};
    if (nodes[_a.node_id].parent_id != nodes[_b.node_id].parent_id)
      set_d.push_back(_b);
    float a_parent_cost = _a.cost;
    float b_parent_cost = _b.cost;
    for (uint32_t i = 0; i < set_d.size(); i++)
    {
      const node_cost& n_c = set_d[i];
      const tree_node& v = nodes[n_c.node_id];
      const float cost_v = n_c.cost;
      const tree_node& v_parent = nodes[v.parent_id];
      float cost_parent = cost_v;
      if (v_parent.right_only || v_parent.left_only)
        continue;
      const tree_node& w = _binary_tree.get_sibling(v);  // w is sibling of v
      float cost_w = return_cost(w);
      if (cost_v != cost_w)
      {
        VW_DBG(_dd) << "otree_c: learn() cost_w = " << cost_w << ", cost_v != cost_w" << std::endl;
        float local_action = RIGHT;
        if (((cost_v < cost_w) ? v : w).id == v_parent.left_id)  ////
          local_action = LEFT;

        ec.l.simple.label = local_action;  // TODO:scalar label type
        ec.l.simple.initial = 0.f;
        ec.weight = abs(cost_v - cost_w);
  
        bool filter = false;
        const float weight_th = 0.00001f;
        if (ec.weight < weight_th)
        {
          // generate a new seed
          uint64_t new_random_seed = uniform_hash(&app_seed, sizeof(app_seed), app_seed);
          // pick a uniform random number between 0.0 - .001f
          float random_draw = exploration::uniform_random_merand48(new_random_seed)*weight_th;
          if (random_draw < ec.weight) {
            ec.weight = weight_th;
          }
          else {
            filter = true;
          }
        }
        if (!filter)
        {
          VW_DBG(_dd) << "otree_c: learn() #### binary learning the node " << v.parent_id << std::endl;
          base.learn(ec, v.parent_id);
          _binary_tree.nodes[v.parent_id].learn_count++;
          base.predict(ec, v.parent_id);
          VW_DBG(_dd) << "otree_c: learn() after binary predict:" << scalar_pred_to_string(ec)
            << ", local_action = " << (local_action) << std::endl;
          float trained_action = (ec.pred.scalar < 0) ? LEFT : RIGHT;
          if (trained_action == local_action)
          {
            cost_parent =
              (std::min)(cost_v, cost_w) * fabs(ec.pred.scalar) + (std::max)(cost_v, cost_w) * (1 - fabs(ec.pred.scalar));
            VW_DBG(_dd) << "otree_c: learn() ec.pred.scalar == local_action" << std::endl;
          }
          else
          {
            cost_parent =
              (std::max)(cost_v, cost_w) * fabs(ec.pred.scalar) + (std::min)(cost_v, cost_w) * (1 - fabs(ec.pred.scalar));
            VW_DBG(_dd) << "otree_c: learn() ec.pred.scalar != local_action" << std::endl;
          }
        }
        
      }
      if (i == 0)
        a_parent_cost = cost_parent;
      else
        b_parent_cost = cost_parent;      
    }
    _a = {nodes[_a.node_id].parent_id, a_parent_cost};
    _b = {nodes[_b.node_id].parent_id, b_parent_cost};
  }

  ec.l = saved_label;
  ec.weight = saved_weight;
  ec.pred = saved_pred;
}

void offset_tree::set_trace_message(std::ostream* vw_ostream) { _trace_stream = vw_ostream; }

offset_tree::~offset_tree() { (*_trace_stream) << tree_stats_to_string() << std::endl; }

std::string offset_tree::tree_stats_to_string()
{
  return _binary_tree.tree_stats_to_string();
}

void predict(offset_tree& ot, single_learner& base, example& ec)
{
  VW_DBG(ec) << "otree_c: before tree.predict() " << multiclass_pred_to_string(ec) << features_to_string(ec)
             << std::endl;
  ec.pred.multiclass = ot.predict(base, ec);  // TODO: check: making the prediction zero-based?
  VW_DBG(ec) << "otree_c: after tree.predict() " << multiclass_pred_to_string(ec) << features_to_string(ec)
             << std::endl;
}

void learn(offset_tree& tree, single_learner& base, example& ec)
{
  VW_DBG(ec) << "otree_c: before tree.learn() " << cb_label_to_string(ec) << features_to_string(ec) << std::endl;
  tree.learn(base, ec);
  VW_DBG(ec) << "otree_c: after tree.learn() " << cb_label_to_string(ec) << features_to_string(ec) << std::endl;
}

void finish(offset_tree& t) { t.~offset_tree(); }

base_learner* offset_tree_cont_setup(VW::config::options_i& options, vw& all)
{
  option_group_definition new_options("Offset tree continuous Options");
  uint32_t num_actions; // = K = 2^D
  uint32_t bandwidth; // = 2^h#
  uint32_t scorer_flag;
  new_options.add(make_option("otc", num_actions).keep().help("Offset tree continuous with <k> labels")) // TODO: D or K
      .add(make_option("scorer_option", scorer_flag)
               .default_value(0)
               .keep()
               .help("Offset tree continuous reduction to scorer [-1, 1] versus binary -1/+1"))  // TODO: oct
      .add(make_option("bandwidth", bandwidth)
               .default_value(0)
               .keep()
               .help("bandwidth for continuous actions in terms of #actions"));  // TODO: h# or 2^h#

  options.add_and_parse(new_options);

  if (!options.was_supplied("otc"))  // todo: if num_actions = 0 throw error
    return nullptr;

  if (scorer_flag)
  {
    options.insert("link", "glf1");
  }
  else  // if (!options.was_supplied("binary"))
  {
    options.insert("binary", "");
  }

  auto otree = scoped_calloc_or_throw<offset_tree>();
  otree->init(num_actions, bandwidth);
  otree->set_trace_message(&all.trace_message);

  base_learner* base = setup_base(options, all);

  // all.delete_prediction = ACTION_SCORE::delete_action_scores; //TODO: commented

  learner<offset_tree, example>& l =
      init_learner(otree, as_singleline(base), learn, predict, otree->learner_count(), prediction_type::multiclass);
  // TODO: changed to prediction_type::multiclass

  l.set_finish(finish);

  return make_base(l);
}

}  // namespace offset_tree_cont
}  // namespace VW