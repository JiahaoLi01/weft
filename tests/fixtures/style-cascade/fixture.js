// Shared immutable browser/native DOM workload. Interaction uses real controls.
(() => {
  const put = (tag, id, parent, attrs = {}, text = '') => {
    const n = document.createElement(tag); n.setAttribute('id',id);
    for (const [k,v] of Object.entries(attrs)) n.setAttribute(k,v);
    if(text) n.appendChild(document.createTextNode(text)); parent.appendChild(n); return n;
  };
  const host = put('div','cases',document.body,{},'');
  for(const c of 'abcdefghijklmnop') put('div','case-'+c,host,{},'Case '+c);
  document.getElementById('case-m').setAttribute('data-label','a,b');
  const theme = put('button','theme',document.body,{},'Change inherited theme');
  theme.addEventListener('click',()=>document.documentElement.setAttribute('class','alternate'));
  const expand = put('button','expand',document.body,{},'Toggle expanded attribute');
  expand.addEventListener('click',()=>document.getElementById('case-l').setAttribute('aria-expanded','true'));
})();
