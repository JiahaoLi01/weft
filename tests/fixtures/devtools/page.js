// The elements of the developer tools' page, in Chrome and in a View alike.
var root = document.getElementById('root');
function add(parent, id, className, style, text) {
  var element = document.createElement('div');
  element.id = id;
  element.className = className;
  element.setAttribute('data-check', '1');
  if (style) {
    element.setAttribute('style', style);
  }
  if (text) {
    element.appendChild(document.createTextNode(text));
  }
  parent.appendChild(element);
  return element;
}
add(root, 'one', 'box strong', '', 'one');
add(root, 'two', 'box', 'height: 24px; column-count: 3', 'two');
var three = add(root, 'three', 'parent', '', '');
add(three, 'four', 'child', '', '');
add(three, 'six', 'only-unknown', '', '');
add(root, 'five', 'multi', '', '');
window.clicks = 0;
root.addEventListener('click', function () {
  window.clicks += 1;
});
