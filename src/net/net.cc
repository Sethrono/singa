// Copyright © 2014 Wei Wang. All Rights Reserved.
// 2014-07-14 13:18
#include "net/net.h"

namespace lapis {
// visited all out going layers and then push current layer into the stack
void Net::topology_sort_inner(Layer *layer,
                         const std::map<Layer *,
                         std::vector<Layer *>> &adjacent_list,
                         std::map<Layer *, bool> *visited,
                         std::stack<Layer *> *stack) {
  (*visited)[layer] = true;
  for (Layer *layer1 : adjacent_list.at(layer)) {
    if ((*visited)[layer1])
      continue;
    topology_sort_inner(layer1, adjacent_list, visited, stack);
  }
  stack->push(layer);
}

// sort to make `bottom' layers be placed in the front positions
// forward propagation will be processed based on this order
void Net::topology_sort(std::vector<Layer *> *layers) {
  // adjacent list from upper layers to lower layers
  std::map<Layer *, std::vector<Layer *>> adjacent_list;
  std::map<Layer *, bool> visited;
  std::vector<Layer *> input_layers;
  // prepare adjacent list; input layers will be processed firstly,
  // hence no need to sort them (mark them as visited)
  for (Layer *layer : *layers) {
    /*
    if (layer->HasInput()) {
      visited[layer] = true;
      input_layers.push_back(layer);
    } else {
      visited[layer] = false;
    }
    */
    // automatically insert a new entry to the map
    // the direction of edge is from layer to layers in adjacent_list
    adjacent_list[layer];
    for (Edge *edge : layer->out_edges()) {
      Layer *layer1 = edge->OtherSide(layer);
      adjacent_list[layer].push_back(layer1);
    }
  }
  // the `top' layer in the net will be placed at the bottom of the stack
  // and then be processed (i.e., forward) at last
  std::stack<Layer *> stack;
  for (Layer *layer : *layers) {
    if (visited[layer] == false)
      topology_sort_inner(layer, adjacent_list, &visited, &stack);
  }
  layers->clear();
  // input layers are placed at front to be processed firstly
  for (auto layer : input_layers)
    layers->push_back(layer);
  while (!stack.empty()) {
    layers->push_back(stack.top());
    stack.pop();
  }
}

Net::Net(const NetProto &net_proto) {
  LOG(INFO)<<"Construct Neural Net...";
  std::map<std::string, Layer *> layer_map;
  for (auto &layer_proto : net_proto.layer()) {
    Layer *layer = LayerFactory::Instance()->Create(layer_proto.type());
    layer->Init(layer_proto);
    layers_.push_back(layer);
    layer_map[layer->name()] = layer;
    if(layer->HasInput())
      input_layer_.push_back(dynamic_cast<InputLayer*>(layer));
    if(layer->HasOutput())
      output_layer_.push_back(dynamic_cast<OutputLayer*>(layer));
  }
  for (auto &edge_proto : net_proto.edge()) {
    Edge *edge = new Edge();
    edge->Init(edge_proto, layer_map);
    edges_.push_back(edge);
  }
  topology_sort(&layers_);
  for(auto* layer: layers_)
    layer->CollectParams(&params_);
  // the softmax loss layer
  LOG(INFO)<<"Neural Net constructed";
}

void Net::Forward() {
  for (auto* layer : layers_)
    layer->ComputeFeature();
}

void Net::Backward() {
  for (auto layer = layers_.rbegin(); layer != layers_.rend(); layer++)
    (*layer)->ComputeGradient();
}


void Net::InitDAryShape(){
  for(auto *layer : layers_) {
    VLOG(3)<<layer->name();
    layer->InitDAryShape();
  }
}

void Net::InitDAryShape(const vector<vector<int>>& shapes){
  for (auto *layer : input_layer_){
    InputLayer* dlayer=dynamic_cast<InputLayer*>(layer);
    dlayer->InitDAryShape(shapes);
  }
  InitDAryShape();
}

void Net::AllocateMemory() {
  for(auto* layer: layers_)
    layer->AllocateMemory();
}

void Net::InitParameters() {
  for(auto* param: params_){
    param->Fill();
  }
}
/**
 * called by worker
 */
void Net::Setup() {
  InitDAryShape();
  AllocateMemory();
}
/**
 * called by coordiator
 */
void Net::Setup(const vector<vector<int>>& input_shapes) {
  InitDAryShape(input_shapes);
  InitParameters();
}

void Net::ToProto(NetProto *proto, bool copyData) {
  for (Layer *layer : layers_) {
    LayerProto *layer_proto = proto->add_layer();
    layer->ToProto(layer_proto,copyData);
  }
  for (Edge *edge : edges_) {
    EdgeProto *edge_proto = proto->add_edge();
    edge->ToProto(edge_proto);
  }
}
Net::~Net() {
  for(auto* layer: layers_)
    delete layer;
  for(auto* edge: edges_)
    delete edge;
}
}  // namespace lapis
