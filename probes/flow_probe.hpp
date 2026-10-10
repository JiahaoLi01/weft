const caseelephant::ProbeDomSnapshot& flow_find(const caseelephant::ProbeDomSnapshot& n,const std::string& id){
    if(n.attributes.contains("id") && n.attributes.at("id")==id)return n;
    for(const auto& c:n.children){try{return flow_find(c,id);}catch(const std::out_of_range&){}}throw std::out_of_range(id);
}
int run_flow(bool warp,const std::filesystem::path& folder){
    std::filesystem::create_directories(folder);NativeReactWindow window(false);Experiment gpu(warp,640,420);
    WindowPresenter presenter(gpu,window.state.hwnd);presenter.resize(640,420);presenter.capture=true;
    caseelephant::TextProbeFontSet fonts({CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
    const auto css=read_text(CE_SOURCE_DIR "/tests/fixtures/flow-layout/profile.css");caseelephant::ScriptDomProbe vm;
    vm.eval(read_text(CE_SOURCE_DIR "/probes/web_host_bootstrap.js"));vm.eval(read_text(CE_SOURCE_DIR "/tests/fixtures/flow-layout/fixture.js"));
    vm.install_layout_host([&](const auto& dom){return caseelephant::build_flow_page_probe(flow_find(dom,"gallery"),css,640,420,fonts).elements;});
    std::array<caseelephant::ProbeFlowPage,3> pages;
    for(int i=0;i<3;++i){if(i)require(vm.dispatch_mouse(vm.node_identity("#advance"),{caseelephant::ProbeMouseType::Click,30,320})==caseelephant::ProbeDispatchResult::DefaultAllowed,"flow native button dispatch");
        pages[i]=caseelephant::build_flow_page_probe(vm.snapshot_subtree("#gallery"),css,640,420,fonts);}
    const auto old=vm.node_identity("#viewport");vm.reset_document();vm.collect_garbage();require(!vm.is_live(old),"flow values survive epoch reset");
    auto images=load_cheetah_images();unsigned retired{},masks{},fallback{};std::size_t samples{};int error{},mask_error{};
    std::ostringstream report;report<<"{\"fixture\":\"flow-layout-gpu-v1\",\"driver\":"<<json_string(warp?"warp":"hardware")<<",\"adapter\":"<<json_string(gpu.adapter)<<",\"frames\":[";
    std::vector<Pixel> previous;
    auto render=[&](const caseelephant::ProbePositionedPage& page){auto packet=cheetah_packet(page,fonts,images);retired+=unsigned(packet.assets.size());gpu.render_window(packet,[&]{gpu.immediate->CopyResource(gpu.staging.Get(),gpu.output.texture.Get());presenter.present();});
        // Inspect the RGBA UI target before the host's opaque-black presentation.
        std::vector<Pixel> pixels(640*420);D3D11_MAPPED_SUBRESOURCE mapped{};checked(gpu.immediate->Map(gpu.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"flow completed RGBA target");
        for(unsigned y=0;y<420;++y)std::memcpy(pixels.data()+y*640,static_cast<std::uint8_t*>(mapped.pData)+y*mapped.RowPitch,640*sizeof(Pixel));gpu.immediate->Unmap(gpu.staging.Get(),0);
        for(int y=0;y<420;y+=3)for(int x=0;x<640;x+=3){auto expected=cheetah_sample(packet,x,y);for(int c=0;c<4;++c)error=std::max(error,std::abs(int(expected[c])-int(pixels[std::size_t(y)*640+x][c])));++samples;}
        return std::pair{std::move(packet),std::move(pixels)};};
    for(int i=0;i<3;++i){for(const auto& p:pages[i].page.paints){masks+=!p.rounded_clips.empty();for(const auto& g:p.paint.shaped.glyphs)fallback+=g.font_id==1;}
        auto [packet,pixels]=render(pages[i].page);require(pixels.front()==Pixel{0,0,0,0},"rounded root corner transparent");require(pixels[10*640+320]==Pixel{20,34,45,255},"rounded root center background");
        if(i)require(pixels!=previous,"flow mutation changes GPU pixels");previous=pixels;save_png(folder/("frame-"+std::to_string(i)+".png"),pixels,640,420);
        if(i)report<<',';report<<"{\"layout\":"<<page_json(pages[i].page.geometry)<<",\"textures\":"<<packet.assets.size()<<'}';}
    // Separate dense area integration does not reuse the mask generation code.
    caseelephant::ProbePositionedPage shape;caseelephant::ProbePositionedPaint solid;
    solid.paint={caseelephant::ProbePaintKind::Rectangle,"ellipse",{},10,10,100,80,0,{1,1,1,1},{}};solid.clip={0,0,640,420};
    solid.rounded_clips={caseelephant::normalize_rounded_probe({{10,10,100,80},{70,70,70,70},{56,56,56,56}})};shape.paints.push_back(solid);
    auto isolated=cheetah_packet(shape,fonts,{});require(isolated.assets.size()==1,"solid coverage mask prepared");const auto& mask=isolated.assets[0]->bytes;
    for(int y=0;y<80;++y)for(int x=0;x<100;++x){unsigned count{};constexpr int dense=64;
        for(int yy=0;yy<dense;++yy)for(int xx=0;xx<dense;++xx){const double dx=(x+(xx+.5)/dense-50)/50,dy=(y+(yy+.5)/dense-40)/40;if(dx*dx+dy*dy<=1)++count;}
        const auto reference=int(std::lround(count*255./(dense*dense)));mask_error=std::max(mask_error,std::abs(reference-int(mask[std::size_t(y)*100+x])));}
    if(mask_error>5)std::cerr<<"ellipse difference "<<mask_error<<'\n';require(mask_error<=5,"dense independent elliptical area oracle");
    shape.paints[0].image_source="art/ink-panel-v2.png";
    auto translucent=solid;translucent.paint.srgb={.2f,.8f,.4f,.35f};translucent.paint.x=50;translucent.paint.y=30;shape.paints.push_back(translucent);
    auto [packet,pixels]=render(shape);save_png(folder/"masked-original-image.png",pixels,640,420);
    require(pixels[10*640+10]==Pixel{0,0,0,0},"masked PNG outside corner");require(error<=3,"flow bilinear linear-premultiplied GPU oracle");
    gpu.check_debug_messages();require(retired==gpu.native_releases,"all textures retire on actual completion");
    report<<"],\"cpu_pixel_samples\":"<<samples<<",\"cpu_pixel_max_difference\":"<<error<<",\"independent_ellipse_area_max_difference\":"<<mask_error<<",\"rounded_paints\":"<<masks<<",\"fallback_glyphs\":"<<fallback<<",\"native_textures_retired\":"<<retired<<",\"actual_completion_query\":true,\"post_vm_reset_render_verified\":true,\"render_backend_abi_version\":1,\"debug_warning_error_count\":"<<(gpu.debug_layer?"0":"null")<<",\"mobile_verified\":false,\"browser_pixel_parity_verified\":false,\"passed\":true}";
    write_text(folder/"report.json",report.str());std::cout<<"PASS flow D3D11: CPU max "<<error<<", ellipse area max "<<mask_error<<", "<<retired<<" retired textures\n";return 0;
}
