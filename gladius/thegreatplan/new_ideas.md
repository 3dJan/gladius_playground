# New Ideas

## Performance improvement for graph to code generation, inline nodes
- Determine chains of nodes that can be grouped together that are linear and only have one output, so we do not have to store intermediate results in variables.

## backwards compatiblity for files that still contain compose matrix nodes with "matrix" output
- Replace with "result" output during loading
- Add warning to log

## Use google benchmark to measure performance of critical sections
- Add benchmarks for critical sections of code

## Use profiling tools to identify bottlenecks
- Integrate profiling tools to analyze performance
- Focus on high-impact areas for optimization

## Decouple UI and Preview rendering to make UI more responsive
- UI still has to run in the main thread
- Preview rendering can be done offscreen in a separate thread
- See detailed analysis in `rendering_coupling_analysis.md`


## Improve Compilation times
- Use precompiled headers for common includes
- Check if the build cache is working properly
- Review architecture to reduce dependencies between modules
- Use more forward declarations instead of includes where possible