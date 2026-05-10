# Todos

## UX
- Remove gray rectangle when loading a model
- Highlight currently used main function, or function dependencies in general when selecting a function in the outline  
    - Light green and bold for all items that depend on the currently selected item
    - Light blue and italic for all items that the currently selected item depends on
    - Tooltip on hover to explain the color coding
    
- After loading switch to the function referenced by the levelset of the first build item that has one 


## Mesh Export
- Manifold Dual Contouring needs to generate watertight meshes when exporting the color shells (it works well without colors)

- Ensure that the model cannot be edited while exporting

- Simplify mesh export settings UI
- Color mapping also with openvdb mesh export
- Simplify the Color Export settings UI
    - Visualize color mapping graphically instead of a table
    - Color Distribution Heat map: Input vs Output 

## Direct STL Import
When no model is loaded and stl file is dropped into the window, open a special template, where we add the mesh and create a levelset from it automatically (mesh to distance node, to make it eas to add something like a gyroid)

## Improve 3mf library browser
- Remove duplicated functions after merging a 3mf file from the library
- Easy way to store to the library
- Merge other resources (textures, materials) when merging a 3mf file from the library
- Define a way to mark a build item just as an usage example
- Defline a way to define what should actually be merged

## MCP Server
- Goal: Agent should be able to extend the library (e.g. a bunch of well known tpms structures)

## Wizards for frequent tasks
- Fill an imported mesh with tpms, optinoally define shell thickness



