// Shared fixture: the unmodified D3 7.9.0 and d3-sankey 0.12.3 UMDs and this script run in a browser and in weft's
// QuickJS/native DOM. No DOM shim is permitted here. A month of one good's flow, laid out by d3.sankey and drawn as a
// game's economy panel draws it: links as paths of d3.sankeyLinkHorizontal stroked as wide as their values (translucent),
// nodes as rects, labels as svg text beside them. The script's completion is JSON text: the layout's numbers and the
// svg as a node tree ({tag, attributes, children}, text as strings), the form of the scene paint cases
// (tests/fixtures/scene-paint-reference/cases.json, whose "sankey" case is this tree, see
// tools/browser_d3_sankey_reference.mjs). The report also remains on globalThis.
(function () {
  "use strict";

  const checks = [];
  function check(condition, name) {
    if (!condition) throw new Error("d3-sankey fixture failed: " + name);
    checks.push(name);
  }

  check(d3.version === "7.9.0", "unmodified D3 7.9.0 loaded");
  check(typeof d3.sankey === "function" && typeof d3.sankeyLinkHorizontal === "function" &&
        typeof d3.sankeyLeft === "function", "d3-sankey on the d3 global");

  const width = 240;
  const height = 100;
  // Three cities' output: what each keeps, what is lost on the way and what reaches the treasury; the treasury's month:
  // last month's stock in, and what spoils, what is sent out to fill shortages (some of it lost) and what it keeps.
  const nodes = [
    {name: "Luoyang", color: "#c8a050"}, {name: "Changan", color: "#c8a050"}, {name: "Wan", color: "#c8a050"},
    {name: "Opening", color: "#7d725b"}, {name: "Local", color: "#7d725b"}, {name: "Lost", color: "#8f4a3a"},
    {name: "Treasury", color: "#d4b06a"}, {name: "Spoiled", color: "#6b5a48"}, {name: "Sent", color: "#56785d"},
    {name: "Kept", color: "#d4b06a"}, {name: "Filled", color: "#56785d"}, {name: "Lost2", color: "#8f4a3a"}
  ];
  const links = [
    {source: "Luoyang", target: "Treasury", value: 30},
    {source: "Changan", target: "Treasury", value: 26.1},
    {source: "Changan", target: "Lost", value: 3.9},
    {source: "Changan", target: "Local", value: 20},
    {source: "Wan", target: "Treasury", value: 8.5},
    {source: "Wan", target: "Lost", value: 2.5},
    {source: "Opening", target: "Treasury", value: 45},
    {source: "Treasury", target: "Spoiled", value: 4.5},
    {source: "Treasury", target: "Sent", value: 12},
    {source: "Treasury", target: "Kept", value: 93.1},
    {source: "Sent", target: "Filled", value: 9.6},
    {source: "Sent", target: "Lost2", value: 2.4}
  ];

  const sankey = d3.sankey()
      .nodeId(d => d.name)
      .nodeAlign(d3.sankeyLeft)
      .nodeWidth(8)
      .nodePadding(6)
      .extent([[1, 4], [width - 1, height - 4]]);
  const graph = sankey({nodes: nodes.map(d => Object.assign({}, d)), links: links.map(d => Object.assign({}, d))});
  check(graph.nodes.length === nodes.length && graph.links.length === links.length, "every node and link laid out");
  const byName = new Map(graph.nodes.map(n => [n.name, n]));
  check(byName.get("Luoyang").depth === 0 && byName.get("Treasury").depth === 1 && byName.get("Sent").depth === 2 &&
        byName.get("Filled").depth === 3, "depths by the links");
  check(Math.abs(byName.get("Treasury").value - 109.6) < 1e-9, "a node's value is the larger of its sums");

  const svg = d3.create("svg")
      .attr("width", width)
      .attr("height", height)
      .attr("viewBox", [0, 0, width, height].join(" "));
  svg.append("g")
      .attr("fill", "none")
      .attr("stroke-opacity", 0.5)
    .selectAll("path")
    .data(graph.links)
    .join("path")
      .attr("d", d3.sankeyLinkHorizontal())
      .attr("stroke", d => d.source.color)
      .attr("stroke-width", d => Math.max(1, d.width));
  svg.append("g")
    .selectAll("rect")
    .data(graph.nodes)
    .join("rect")
      .attr("x", d => d.x0)
      .attr("y", d => d.y0)
      .attr("width", d => d.x1 - d.x0)
      .attr("height", d => Math.max(1, d.y1 - d.y0))
      .attr("fill", d => d.color);
  svg.append("g")
      .attr("font-family", "Arial")
      .attr("font-size", 7)
      .attr("fill", "#202020")
    .selectAll("text")
    .data(graph.nodes.filter(d => d.y1 - d.y0 >= 6))
    .join("text")
      .attr("x", d => d.x0 < width / 2 ? d.x1 + 2 : d.x0 - 2)
      .attr("y", d => (d.y0 + d.y1) / 2)
      .attr("dy", "0.35em")
      .attr("text-anchor", d => d.x0 < width / 2 ? "start" : "end")
      .text(d => d.name);

  // The svg as a node tree: tag, the attributes D3 set (in a fixed order), children, text.
  const names = ["width", "height", "viewBox", "fill", "stroke", "stroke-width", "stroke-opacity", "font-family",
                 "font-size", "d", "x", "y", "dy", "text-anchor"];
  function tree(node) {
    if (node.nodeType === 3) return node.data;
    const out = {tag: node.localName};
    const attributes = {};
    for (const name of names) {
      if (node.hasAttribute(name)) attributes[name] = node.getAttribute(name);
    }
    if (Object.keys(attributes).length) out.attributes = attributes;
    const children = Array.from(node.childNodes).map(tree);
    if (children.length) out.children = children;
    return out;
  }
  const root = svg.node();
  check(root.namespaceURI === "http://www.w3.org/2000/svg", "svg namespace");
  check(root.querySelectorAll("path").length === links.length && root.querySelectorAll("rect").length === nodes.length,
        "a path for each link, a rect for each node");

  const result = {
    fixture: "weft-d3-sankey-v1",
    d3_version: d3.version,
    checks_passed: checks.length,
    checks,
    nodes: graph.nodes.map(n => ({name: n.name, depth: n.depth, value: n.value, x0: n.x0, x1: n.x1, y0: n.y0, y1: n.y1})),
    links: graph.links.map(l => ({source: l.source.name, target: l.target.name, value: l.value, width: l.width,
                                  y0: l.y0, y1: l.y1})),
    svg: tree(root)
  };
  globalThis.weftSankeyReport = result;
  return JSON.stringify(result);
})();
