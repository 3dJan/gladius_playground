# Optimized Layered Layout Algorithm

X Axis should represent topological order of nodes
Nodes have a order based on their topological depth in graph, the begin node has 1, the end node has the highest order. We want to layout nodes so that for each node no node with a lower order has a greater X cooordinate than itself. although nodes should be as far right as possible to minimize edge lengths. we can use the layered layout algorithm to determine the X positions of nodes based on their order. afterwards we can try to push nodes to the right as much as possible without violating the order constraint. Each "layer" is a column.

Y Axis
We start with the column with the highest order and iterate down to the lowest order
(right to left).
Determine for each node the y position of all consuming nodes. Calculate the median y position of all consuming nodes. Place the node at that y position if possible (overlaps with other nodes are forbidden). If not possible find the nearest free y position.
Repeat for all nodes of a column.
When all nodes of a column are placed, optimize the y positions iteratively to minimize edge lengths.

Repeat for all columns (right to left).
