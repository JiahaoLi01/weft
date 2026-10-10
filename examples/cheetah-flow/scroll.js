(function(){
  const button=document.createElement('button');button.id='flow-scroll';button.textContent='滚动展开列表 / Scroll list';
  button.addEventListener('click',()=>{const list=document.querySelector('.alert-list');if(list)list.scrollTop=70;});
  document.body.appendChild(button);
})();
