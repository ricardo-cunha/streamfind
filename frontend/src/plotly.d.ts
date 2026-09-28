declare module 'plotly.js-dist-min' {
  const Plotly: {
    newPlot: (
      element: HTMLElement,
      data: unknown[],
      layout?: Record<string, unknown>,
      config?: Record<string, unknown>,
    ) => void | Promise<unknown>;
    purge: (element: HTMLElement) => void;
  };
  export default Plotly;
}
