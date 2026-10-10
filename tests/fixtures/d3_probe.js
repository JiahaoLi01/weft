// Shared fixture: the same unmodified D3 UMD and this script run in a browser
// and in CaseElephant's QuickJS/native DOM. No DOM shim is permitted here.
// Script completion is JSON text; the report also remains on globalThis.
(function () {
  "use strict";

  const checks = [];
  function check(condition, name) {
    if (!condition) throw new Error("D3 fixture failed: " + name);
    checks.push(name);
  }
  function near(actual, expected) {
    return Math.abs(Number(actual) - expected) < 1e-9;
  }
  function keys(selection) {
    return selection.nodes().map(node => node.getAttribute("data-key"));
  }

  check(d3.version === "7.9.0", "unmodified D3 7.9.0 loaded");
  const SVG = "http://www.w3.org/2000/svg";
  const HTML = "http://www.w3.org/1999/xhtml";
  const XLINK = "http://www.w3.org/1999/xlink";
  const initial = [
    { id: "a", label: "Alpha", value: 25 },
    { id: "b", label: "Beta", value: 50 },
    { id: "c", label: "Gamma", value: 75 }
  ];
  const updated = [
    { id: "c", label: "Gamma", value: 60 },
    { id: "d", label: "Delta", value: 90 },
    { id: "a", label: "Alpha", value: 40 }
  ];

  const host = d3.select("body").append("div").attr("id", "d3-probe");
  check(host.node().namespaceURI === HTML, "HTML namespace inherited");
  check(d3.select("#d3-probe").node() === host.node(), "document selector and wrapper identity");

  const svg = d3.create("svg")
      .attr("id", "d3-chart")
      .attr("width", 480)
      .attr("height", 260)
      .attr("viewBox", "0 0 480 260");
  host.append(() => svg.node());
  const plot = svg.append("g").attr("transform", "translate(40,20)");
  check(svg.node().namespaceURI === SVG && plot.node().namespaceURI === SVG,
      "create svg and inherited SVG namespace");
  check(svg.attr("viewBox") === "0 0 480 260", "SVG attribute case preserved");
  const x = d3.scaleBand().domain(initial.map(d => d.id)).range([0, 400]).padding(0.2);
  const y = d3.scaleLinear().domain([0, 100]).range([200, 0]);
  check(near(x("a"), 25) && near(x("b"), 150) && near(x.bandwidth(), 100), "band scale positions");
  check(x("missing") === undefined && near(y(25), 150) && near(y.invert(150), 25),
      "unknown band and linear invert");
  const axisGroup = plot.append("g").attr("class", "axis").attr("transform", "translate(0,200)");
  const axis = d3.axisBottom(x).tickSizeOuter(0).offset(0.5);
  axisGroup.call(axis);
  const initialTicks = axisGroup.selectAll("g.tick").nodes();
  check(initialTicks.map(node => node.__data__).join(",") === "a,b,c", "D3 axis creates SVG ticks");

  function render(data) {
    x.domain(data.map(d => d.id));
    return plot.selectAll("rect.bar")
        .data(data, d => d.id)
        .join(
          enter => enter.append("rect").attr("class", "bar"),
          update => update,
          exit => exit.remove()
        )
        .attr("data-key", d => d.id)
        .attr("x", d => x(d.id))
        .attr("y", d => y(d.value))
        .attr("width", x.bandwidth())
        .attr("height", d => 200 - y(d.value))
        .attr("fill", "#4089c7");
  }

  const firstBars = render(initial).nodes();
  const retainedA = firstBars[0];
  const removedB = firstBars[1];
  const retainedC = firstBars[2];
  retainedA.applicationState = { visits: 3 };
  const bars = render(updated);
  const ordered = plot.selectAll("rect.bar").nodes();
  check(keys(bars).join(",") === "c,d,a", "keyed selection order");
  check(keys(plot.selectAll("rect.bar")).join(",") === "c,d,a", "keyed DOM reorder and enter before update");
  check(ordered[0] === retainedC && ordered[2] === retainedA && ordered[1] !== removedB,
      "keyed update preserves existing identities");
  check(retainedA.applicationState.visits === 3 && retainedA.__data__.value === 40,
      "wrapper expando and datum survive reselect");
  check(removedB.parentNode === null && removedB.__data__.id === "b", "exit removes but retained node stays valid");
  d3.select(removedB).attr("data-retained", "yes");
  check(removedB.getAttribute("data-retained") === "yes", "detached node remains mutable");
  check((retainedC.compareDocumentPosition(retainedA) & 4) !== 0,
      "compareDocumentPosition following bit");
  check((retainedA.compareDocumentPosition(retainedC) & 2) !== 0,
      "compareDocumentPosition preceding bit");
  check(near(ordered[0].getAttribute("x"), 25) && near(ordered[0].getAttribute("y"), 80)
      && near(ordered[0].getAttribute("height"), 120), "SVG update changes geometry attributes");
  axisGroup.call(axis);
  const ticks = axisGroup.selectAll("g.tick").nodes();
  check(ticks.map(node => node.__data__).join(",") === "c,d,a"
      && ticks[0] === initialTicks[2] && ticks[2] === initialTicks[0] && initialTicks[1].parentNode === null,
      "D3 axis keyed update order identity and exit");
  check(axisGroup.selectAll("text").nodes().map(node => node.textContent).join(",") === "c,d,a"
      && axisGroup.select("path.domain").attr("d") === "M0.5,0.5H400.5", "D3 axis text and domain path");
  const lineData = d3.line().x(d => x(d.id) + x.bandwidth() / 2).y(d => y(d.value))(updated);
  const trend = plot.append("path").attr("class", "trend").attr("d", lineData)
      .attr("fill", "none").attr("stroke", "#e78b2f").attr("stroke-width", 2);
  check(lineData === "M75,80L200,20L325,120" && trend.attr("d") === lineData,
      "D3 line generates and assigns SVG path");

  // These elements are future rendering fixtures already, not claims that the
  // native renderer currently interprets the attributes or draws any pixels.
  plot.append("line").attr("class", "baseline")
      .attr("x1", 0).attr("y1", 200).attr("x2", 400).attr("y2", 200)
      .attr("stroke", "#6b7280");
  plot.selectAll("text.label").data(updated, d => d.id).join("text")
      .attr("class", "label")
      .attr("x", d => x(d.id) + x.bandwidth() / 2)
      .attr("y", 225).attr("text-anchor", "middle")
      .attr("fill", "#243145").text(d => d.label);
  check(plot.selectAll("text.label").nodes().map(n => n.textContent).join(",") === "Gamma,Delta,Alpha",
      "SVG text content");
  const marker = plot.append("circle").attr("cx", 375).attr("cy", 20).attr("r", 5).attr("fill", "#e78b2f");
  check(marker.node().namespaceURI === SVG && marker.attr("r") === "5", "circle namespace and numeric attribute conversion");
  marker.attr("xlink:href", "#d3-chart");
  check(marker.attr("xlink:href") === "#d3-chart"
      && marker.node().getAttributeNS(XLINK, "href") === "#d3-chart", "namespaced attribute set and get");
  marker.attr("xlink:href", null);
  check(marker.node().getAttributeNS(XLINK, "href") === null, "namespaced attribute removal");

  const list = host.append("div").attr("class", "legend");
  function renderLegend(data) {
    return list.selectAll("div.row").data(data, d => d.id).join("div")
        .attr("class", "row").attr("data-key", d => d.id)
        .text(d => d.label + ": " + d.value);
  }
  const initialRows = renderLegend(initial).nodes();
  renderLegend(updated);
  const rows = list.selectAll("div.row").nodes();
  check(keys(list.selectAll("div.row")).join(",") === "c,d,a", "HTML keyed reorder");
  check(rows[0] === initialRows[2] && rows[2] === initialRows[0] && initialRows[1].parentNode === null,
      "HTML keyed identity and exit");
  check(rows[1].namespaceURI === HTML && rows[1].textContent === "Delta: 90", "HTML attribute and text update");

  let firstListener = 0;
  let replacementListener = 0;
  let parentListener = 0;
  let correctEventIdentity = true;
  let correctDatum = true;
  let canceledObserved = false;
  plot.on("probe.parent", function (event) {
    ++parentListener;
    correctEventIdentity = correctEventIdentity && event.currentTarget === plot.node() && event.target === retainedC;
    canceledObserved = canceledObserved || event.defaultPrevented;
  });
  const eventBar = d3.select(retainedC);
  eventBar.on("probe.chart", function (event, datum) {
    ++firstListener;
    correctEventIdentity = correctEventIdentity && this === retainedC && event.currentTarget === retainedC;
    correctDatum = correctDatum && datum.id === "c" && datum.value === 60 && event.detail === 11;
    event.preventDefault();
  });
  eventBar.dispatch("probe", { bubbles: true, cancelable: true, detail: 11 });
  eventBar.on("probe.chart", function () { ++replacementListener; });
  eventBar.dispatch("probe", { bubbles: true, cancelable: true, detail: 12 });
  eventBar.on("probe.chart", null);
  eventBar.dispatch("probe", { bubbles: true, detail: 13 });
  plot.on("probe.parent", null);
  check(firstListener === 1 && replacementListener === 1, "D3 listener replacement and removal");
  check(parentListener === 3 && correctEventIdentity && correctDatum && canceledObserved,
      "D3 dispatch datum detail identity bubbling and cancellation");

  const result = {
    fixture: "caseelephant-d3-dom-v1",
    d3_version: d3.version,
    checks_passed: checks.length,
    checks,
    svg_order: keys(plot.selectAll("rect.bar")),
    html_order: keys(list.selectAll("div.row")),
    svg_heights: plot.selectAll("rect.bar").nodes().map(n => Number(n.getAttribute("height"))),
    axis_order: ticks.map(n => n.__data__),
    line_path: lineData,
    html_text: rows.map(n => n.textContent),
    event_calls: [firstListener, replacementListener, parentListener],
    native_rendering_verified: false
  };
  globalThis.caseElephantD3Report = result;
  return JSON.stringify(result);
})();
