/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
 http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License. */

import Foundation

precedencegroup ChainNode {
    associativity: left
    higherThan: MultiplicationPrecedence
}

infix operator --> : ChainNode

class Node {
    var inputs: [Node] = []
    var outputs: [Node] = []
    var type: String
    var opDesc: OpDesc?
    init(inOpDesc: OpDesc) {
        type = inOpDesc.type
        opDesc = inOpDesc
    }
    
    init(inType: String) {
        type = inType
    }
    
    static func -->(lNode: Node, rNode: Node) -> Node {
        lNode.outputs.append(rNode)
        rNode.inputs.append(lNode)
        return rNode
    }
    
    func depth(begin: UInt = 1) -> UInt {
        var beginMax: UInt = 1
        for output in outputs {
            let subDepth = output.depth(begin: begin + 1)
            beginMax = max(begin, subDepth)
        }
        beginMax = max(begin, beginMax)
        return beginMax
    }
    
    func to(depth: UInt) -> Node {
        let beginNode = Node.init(inType: type)
        to(depth: depth - 1, withNode: beginNode)
        return beginNode
    }
    
    func folderWith(fusion: Fusion.Type, removedNodes: inout [Node]) {
        let fusionNode = fusion.fusionNode()
        let change = fusion.change()
        let inOutputs = outputs
        outputs.removeAll()
        opDesc?.outputs.removeAll()
        for i in 0..<inOutputs.count {
            inOutputs[i].folderWith(beginNode: self, matchNode: fusionNode.outputs[i], change: change, removedNodes: &removedNodes)
        }
        opDesc?.type = fusion.fusionType()
        type = fusion.fusionType()
    }
    
    private func folderWith(beginNode: Node, matchNode: Node, change: [String : [(from: String, to: String)]], removedNodes: inout [Node]) {
        guard let inOpdesc = opDesc else {
            fatalError()
        }
        
        for attr in inOpdesc.attrs {
            beginNode.opDesc?.attrs[attr.key] = attr.value
//            print(beginNode.opDesc?.attrs)
        }
        
        for paraInput in inOpdesc.paraInputs {
            if let inChanges = change[type] {
                for keyChange in inChanges {
                    if keyChange.from == paraInput.key {
                        beginNode.opDesc?.paraInputs[keyChange.to] = paraInput.value
                    } else {
                        beginNode.opDesc?.paraInputs[paraInput.key] = paraInput.value
                    }
                }
            } else {
                beginNode.opDesc?.paraInputs[paraInput.key] = paraInput.value
            }
        }
        
        if matchNode.outputs.count == 0 {
            beginNode.outputs.append(contentsOf: outputs)
            beginNode.opDesc?.outputs = inOpdesc.outputs
            
        }
        removedNodes.append(self)
        
        for i in 0..<matchNode.outputs.count {
            outputs[i].folderWith(beginNode: beginNode, matchNode: matchNode.outputs[i], change: change, removedNodes: &removedNodes)
        }
        
    }
    
    private func to(depth: UInt, withNode: Node) {
        if depth < 1 {
            return
        }
        
        for output in outputs {
            let node = Node.init(inType: output.type)
            withNode.outputs.append(node)
            output.to(depth: depth - 1, withNode: node)
        }
    }
    
    
}

extension Node: Equatable {
    static func == (lhs: Node, rhs: Node) -> Bool {
        if lhs.outputs.count != rhs.outputs.count {
            return false
        }
        
        if lhs.type != rhs.type {
            return false
        }
        
        for i in 0..<lhs.outputs.count {
            if lhs.outputs[i] != rhs.outputs[i] {
                return false
            }
        }
        return true
    }
    
}

class ProgramOptimize<P: PrecisionType> {
    // register fusion
    let fusionOps: [Fusion.Type] = [ConvAddBatchNormReluOp<P>.self,
                                    ConvAddOp<P>.self,
                                    ConvBNReluOp<P>.self,
                                    DwConvBNReluOp<P>.self]
    
    func optimize(originProgramDesc: ProgramDesc) -> ProgramDesc {
        
        guard originProgramDesc.blocks.count == 1 else {
            fatalError(" not support yet")
        }
        
        var mapForNodeChain: [String : Node] = [:]
        var nodes: [Node] = []
        var typeMapNodes: [String : [Node]] = [:]
        let block = originProgramDesc.blocks[0]
            for opDesc in block.ops {
                guard let opInputKeys = opInfos[opDesc.type]?.inputs, let outputKeys = opInfos[opDesc.type]?.outputs else {
                    fatalError()
                }
                
                let node = Node.init(inOpDesc: opDesc)
                for inputKey in opInputKeys {
                    if let inputs = opDesc.inputs[inputKey] {
                        for input in inputs {
                            if let inputNode = mapForNodeChain[input] {
                                _ = inputNode --> node
                            }
                        }
                    }
                }
                
                for outputKey in outputKeys {
                    if let outputs = opDesc.outputs[outputKey] {
                        for output in outputs {
                            mapForNodeChain[output] = node
                        }
                    }
                }
                
                nodes.append(node)
                
                if var inNodes = typeMapNodes[opDesc.type] {
                    inNodes.append(node)
                    typeMapNodes[opDesc.type] = inNodes
                } else {
                    typeMapNodes[opDesc.type] = [node]
                }
            }
            
            for fusion in fusionOps {
                let fusionNode = fusion.fusionNode()
                let depth = fusionNode.depth()
                if let toMatchNodes = typeMapNodes[fusionNode.type] {
                    for node in toMatchNodes {
                        let toNode = node.to(depth: depth)
                        if toNode == fusionNode {   // match
                            var removeNodes: [Node] = []
                            node.folderWith(fusion: fusion, removedNodes: &removeNodes)
                            for removeNode in removeNodes {
                                nodes.remove(element: removeNode)
                            }
                        }
                    }
                }
            }
        
        var ops: [OpDesc] = []
        for node in nodes {
            ops.append(node.opDesc!)
        }
        
        var newProgramDesc = ProgramDesc.init()
        let newBlock = BlockDesc.init(inVars: block.vars, inOps: ops)
        newProgramDesc.blocks.append(newBlock)
        return newProgramDesc
    }
}