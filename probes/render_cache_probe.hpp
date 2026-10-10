int run_render_cache(bool warp,const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    Experiment cached(warp,64,64),reference(warp,64,64);cached.cache_assets=true;cached.cache_resource_limit=8;
    // Traffic past the former 64 MiB threshold must preserve live IDs.
    auto stable=std::make_shared<const Asset>(Asset{1,1,Encoding::Coverage,{255}});const auto stable_id=cached.asset_identity(stable);
    for(unsigned i=0;i<24;++i){auto transient=std::make_shared<const Asset>(Asset{1024,1024,Encoding::SrgbStraight,std::vector<std::uint8_t>(4*1024*1024,255)});cached.asset_identity(transient);require(cached.asset_identity(stable)==stable_id,"live texture identity changed under animated bitmap traffic");}
    std::uint64_t previous{};for(unsigned i=0;i<3000;++i){auto transient=std::make_shared<const Asset>(Asset{1,1,Encoding::Coverage,{255}});auto id=cached.asset_identity(transient);require(id>previous && cached.asset_identity(transient)==id,"recycled asset address aliased dead version");previous=id;}
    require(cached.asset_identity(stable)==stable_id && cached.asset_identities.size()<=2048,"identity metadata lost bounded stable asset");
    caseelephant::TextProbeFontSet fonts({CE_SOURCE_DIR "/third_party/harfbuzz/source/perf/fonts/Roboto-Regular.ttf",CE_SOURCE_DIR "/third_party/noto-cjk/source/NotoSansCJKsc-Regular.otf"});
    CheetahPacketCache cpu;
    std::map<std::string,std::shared_ptr<const Asset>> images;
    auto image=[&](unsigned state){auto a=std::make_shared<Asset>(Asset{4,4,Encoding::SrgbStraight,std::vector<std::uint8_t>(64)});
        for(unsigned i=0;i<16;++i){a->bytes[i*4]=std::uint8_t(state*19+i*7);a->bytes[i*4+1]=std::uint8_t(230-state*5);a->bytes[i*4+2]=80;a->bytes[i*4+3]=i%3?255:0;}images["test"]=std::move(a);};image(0);
    auto page=[&](unsigned state){
        caseelephant::ProbePositionedPage out;caseelephant::ProbePositionedPaint item;
        item.paint.kind=caseelephant::ProbePaintKind::Rectangle;item.paint.width=item.paint.height=64;item.paint.srgb={.1f,.1f,.1f,1};item.clip={0,0,64,64};out.paints.push_back(item);
        const float x=4+float(state%5),y=4+float(state%7),r=3+float(state%4);
        auto clip=caseelephant::normalize_rounded_probe({{x,y,50,46},{r,r,r,r},{r+2,r+2,r+2,r+2}});
        item.paint.x=x;item.paint.y=y;item.paint.width=50;item.paint.height=46;item.paint.srgb={.2f,.6f,.4f,.6f};item.rounded_clips={clip};out.paints.push_back(item);
        item.image_source="test";item.paint.x=x-2;item.paint.y=y-2;item.paint.width=55;item.paint.height=48;item.uv={.1f,.05f,.8f,.85f};out.paints.push_back(item);
        item.image_source.clear();item.paint.kind=caseelephant::ProbePaintKind::Text;item.paint.x=8;item.paint.y=32;item.paint.font_size=12+float(state%3);
        item.paint.srgb={.9f,.8f,.2f,1};item.paint.shaped=fonts.shape("A仓",item.paint.font_size).shaped;out.paints.push_back(item);
        if(state%2)std::swap(out.paints[1],out.paints[2]);return out;
    };
    auto read=[&](Experiment& renderer,FramePacket packet){renderer.render_window(std::move(packet),[&]{renderer.immediate->CopyResource(renderer.staging.Get(),renderer.output.texture.Get());});
        std::vector<Pixel> pixels(64*64);D3D11_MAPPED_SUBRESOURCE mapped{};checked(renderer.immediate->Map(renderer.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"cache completed readback");
        for(UINT y=0;y<64;++y)std::memcpy(pixels.data()+y*64,static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch,64*sizeof(Pixel));renderer.immediate->Unmap(renderer.staging.Get(),0);return pixels;};
    // Failure records no executable upload. Retrying a cached Created version
    // must still upload its bytes; a cache entry alone cannot mean Ready.
    auto first=cheetah_packet(page(0),fonts,images,&cpu);auto& backend=cached.ensure_backend();backend.inject_recording_failure=true;bool failed{};
    try{cached.render_window(first,[]{});}catch(const std::runtime_error&){failed=true;}backend.inject_recording_failure=false;
    require(failed && !cached.tracker.Stats().encoded_tokens && !cached.tracker.Stats().submitted_tokens,"cached partial recording failure pins");
    for(const auto& [key,entry]:cached.asset_cache){ResourceInfo info;lifetime(cached.tracker.InspectResource(entry.native.id,info),"cache failed upload state");require(info.phase==ResourcePhase::Created,"failed cached upload became Ready");}
    const auto retry=read(cached,first),fresh=read(reference,cheetah_packet(page(0),fonts,images));int max_error{};
    auto compare=[&](const auto& a,const auto& b){for(std::size_t i=0;i<a.size();++i)for(unsigned c=0;c<4;++c)max_error=std::max(max_error,std::abs(int(a[i][c])-int(b[i][c])));require(max_error<=1,"cached/full mutation pixels differ");};compare(retry,fresh);
    // Deterministic mutations change clips/ellipses, fontsize, font IDs,
    // position, source versions and transparent paint order. Skip unpublished
    // packets as well: the next input must remain complete by itself.
    std::uint32_t seed=0xCE902026;unsigned frames{};
    for(unsigned i=0;i<48;++i){seed=seed*1664525+1013904223;const auto state=seed%31;if(i%9==0)image(i+1);
        if(i%11==0){auto dropped=cheetah_packet(page(state+1),fonts,images,&cpu);require(!dropped.draws.empty(),"dropped packet prepared");}
        compare(read(cached,cheetah_packet(page(state),fonts,images,&cpu)),read(reference,cheetah_packet(page(state),fonts,images)));++frames;
    }
    require(cached.cache_hits>0 && cached.native_releases>0 && cached.asset_cache.size()<=8,"cache reuse/eviction pressure absent");
    // A packet with more simultaneous dependencies than this deliberately
    // tiny cache must fail without an orphan tracked lease. Drain and reuse.
    FramePacket over_capacity;
    for(unsigned i=0;i<9;++i)over_capacity.assets.push_back(std::make_shared<const Asset>(Asset{1,1,Encoding::Coverage,{std::uint8_t(i+1)}}));
    PacketView over_view(over_capacity,cached.window_serial+1,&cached);ce_render_handle rejected{};
    require(ce_renderer_prepare(backend.renderer,&over_view.frame,&rejected)==CE_RENDER_BACKEND_ERROR && !rejected.slot && !cached.tracker.Stats().prepared_frames,"cache capacity rejection retained prepared frame");
    cached.evict_assets(0,true);
    require(!cached.tracker.Stats().resources && cached.native_creations==cached.native_releases,"cache capacity rejection orphaned texture lease");
    // Actual submitted work survives a host exception. Cache teardown must
    // reject until its event query proves completion.
    failed=false;try{cached.render_window(cheetah_packet(page(1),fonts,images,&cpu),[]{throw std::runtime_error("cache-post-submit");});}catch(const std::runtime_error& e){failed=std::string(e.what())=="cache-post-submit";}
    require(failed && cached.submitted && !cached.acknowledged,"cache post-submit failure lost submission");bool early{};try{cached.evict_assets(0,true);}catch(const std::runtime_error&){early=true;}require(early,"cache released before actual completion");
    cached.complete();cached.evict_assets(0,true);reference.evict_assets(0,true);
    require(cached.native_creations==cached.native_releases && !cached.tracker.Stats().resources,"cached texture leak after query drain");
    lifetime(cached.tracker.Shutdown(),"cached shutdown");lifetime(reference.tracker.Shutdown(),"reference shutdown");cached.check_debug_messages();reference.check_debug_messages();
    std::ostringstream report;report<<"{\"fixture\":\"render-cache-v1\",\"driver\":"<<json_string(warp?"warp":"hardware")<<",\"mutation_frames\":"<<frames<<",\"seed\":\"0xCE902026\",\"compared_pixels\":"<<(frames+1)*4096
        <<",\"max_difference\":"<<max_error<<",\"native_creations\":"<<cached.native_creations<<",\"native_releases\":"<<cached.native_releases<<",\"cache_hits\":"<<cached.cache_hits<<",\"capacity_failure_verified\":true,\"partial_retry_verified\":true,\"post_submit_drain_verified\":true,\"debug_warning_error_count\":0,\"passed\":true}";
    write_text(folder/"report.json",report.str());std::cout<<"PASS cached/full paint mutation, source/font/clip/order, eviction and failure drain; max "<<max_error<<'\n';return 0;
}
