// View models pushed by UE (BloodMoneyViewModels.cpp). Money is in pounds; shares and ratios are 0..1.

export type Market = { id: string; name: string; hub?: string; lon: number; lat: number };
export type Geography = { neighborCount?: number; kind?: string; source?: string; province?: string; state?: string; country?: string; countryId?: string; market?: string; marketId?: string; status: string; stateStatus?: string; countryStatus?: string };
export type SeaRoute = { name: string; enabled: boolean; description: string; historicalOpening?: string; requiredProject?: string };
export type Selection = { type: 'country' | 'market' | 'province'; id?: string; name?: string; provinceCount?: number; country?: string; hub?: string };
export type MapMetric = { numeric: boolean; name: string; unit: string; context: string; scope: string; text: string; hasValue: boolean; min: string; max: string; low: string; high: string; missing: string;
  /** The hovered plot's line, the legend's hatch and whether the plot is hatched (the build view). */
  note?: string; hatch?: string; hatched?: boolean };
/** 一项手艺（Production.md 第 1 节）：对一项生产技术的熟练度（0..100）；part 是它所在的工序或部件；inUse：线上在用，否则在渐忘。 */
export type CraftView = { id: string; name: string; part: string; value: number; inUse: boolean };
export type CrewView = { id: string; heads: number; origin: string; effective: number; crafts: CraftView[] };
export type LineGood = { good: string; unit: string; nominal: number; produced: number; unitCost: number; inputs: { good: string; unit: string; perUnit: number }[] };
/** 整处设施的合计（地块与公司的设施列表）：每种商品在各条线上的产量合计、班组与开支；lines = 产线数。 */
export type LineView = { shift: string; shiftName: string; process: string; required: number; heads: number; effective: number; limit: string; lines?: number; goods: LineGood[]; materials: number; conversion: number; piece: number; wages: number; upkeep: number; crews: CrewView[] };
/** 产线上的一个产品与它占的份额。 */
export type LinePart = { productId: string; product: string; brand: string; share: number; ready?: boolean };
/** 一条产线：只做一种商品（同一设计的几个产品可以分它），有自己的生产方式、班次、班组与工程。没有运行状态的线（刚建成）只有前半部分。 */
/** 进货：what was bought of an input from one source; kind is own, lot, farm, import or outside. Cost includes the carting. */
export type InputBuy = { good: string; unit: string; source: string; kind: string; quantity: number; cost: number; haul: number };
/** An input of a line: per unit of its good, at its cheapest source now (delivered to the works), and what that comes to a unit. */
export type LineInput = { good: string; unit: string; perUnit: number; price?: number; source?: string; cost?: number };
export type LineCard = { id: number; key: string; name: string; goodId: string; good: string; unit: string; size: number; parts: LinePart[]; status?: string;
  method?: string; methodId?: string; process: string; idle: boolean; installing: boolean; project?: ProjectView;
  shift?: string; shiftName?: string; required?: number; heads?: number; effective?: number;
  /** 产量对额定的比例（工序形态：最慢那道按班组熟练度的能力）。 */
  pace?: number; limit?: string; nominal?: number; produced?: number; unitCost?: number;
  inputs?: LineInput[]; bought?: InputBuy[]; weekBought?: InputBuy[]; lastWeekBought?: InputBuy[];
  materials?: number; conversion?: number; piece?: number; wages?: number; upkeep?: number; crews?: CrewView[];
  /** 线上在用的手艺，按班组人数平均。 */
  crafts?: CraftView[];
  history?: LineWeek[]; shifts?: ShiftPreview[]; current?: LineFigures; methods?: MethodOption[]; allocation?: LineAllocation;
  /** 产品形态（ProductForms.md）：choice、pack、stages；工序形态的线还有它做的产品的工序链（在这处工厂的实际值）。 */
  form?: string; chain?: StageChain };
/** 工序形态（Processes.md 第 9 节）：一道工序的做法、每级能力与人手、是不是瓶颈；可选工序不做时 present 为 false。 */
export type StageCard = { id: string; name: string; required: boolean; present: boolean; way?: string; wayName?: string; manual?: boolean;
  capacity?: number; crew?: number; bottleneck?: boolean; powered?: boolean; power?: string; detail?: string;
  /** 产线上：班组对这道做法的熟练度（0..100），和按它算的实际每级能力（额定 × (0.5 + 0.5 × 熟练度)）。 */
  skill?: number; actual?: number };
/** 一条工序链：每级能力（瓶颈）、人手、搬运、要的动力；在工厂里还有它的动力与传送，产品设计里是额定值（不含搬运与动力加成）。 */
export type StageChain = { capacity: number; crew: number; porters: number; needs?: string; power?: string; conveyance?: string; product?: string; productId?: string;
  /** 产线上：各道按班组的熟练度算以后，最慢那道的每级能力。 */
  actual?: number; stages: StageCard[] };
/** 手艺换算（Production.md 第 1 节）：换上的做法（to）与它换掉的（from，加工序时没有），班组现在与到时的熟练度。 */
export type CraftChange = { to: string; from?: string; part?: string; before: number; after: number };
export type StageFigures = { capacity: number; crew: number };
/** 产品设计里一道工序的一种做法（额定的每级能力与人手，要的动力档）。 */
export type StageWayOption = { id: string; name: string; effects: string; detail?: string; known: boolean; manual: boolean; capacity: number; crew: number; tier: number;
  power?: string };
/** 产品设计里的一道工序：必需的只能换做法，可选的可以不做；needs 是它要先有的工序。 */
export type StageSlot = { id: string; name: string; required: boolean; opening?: string; needs: string[]; needNames?: string; ways: StageWayOption[] };
/** 工厂一级的动力、传送：现在的做法；经营者还有能换的做法、报价与换上以后各线合计的日产与人手。 */
export type WorksWay = { id: string; name: string; effects: string; known: boolean; quote?: Quote; after?: StageFigures };
export type WorksStage = { stage: string; name: string; way: string; wayName?: string; effects?: string; now?: StageFigures; ways?: WorksWay[] };

// ---- 第四步：知识产权、项目、契约 ----
export type Quote = { ok: boolean; reason: string; kind?: string; engineeringDays?: number; total: number; dueNow: number; building: number; machines: number; install: number; engineering: number; writeOff: number; days: number; scrap: number; crafts?: CraftChange[]; distance: number; orders: string;
  orderRows?: { good: string; unit: string; units: number; supplierId: string; supplier: string; unitPrice: number; freight: number; leadDays: number; attributes: string }[]; dropped?: string[];
  /** 工序形态的改型：停下来装机器的线，照常生产、完工那天换上新链的线。 */
  stops?: string[]; runOn?: string[] };
/** Engineering state of a product: a new design goes on a line only once it is ready. */
export type ReadyState = { ready?: boolean; readyDate?: string };
/** Where a product is made: works and its share of each. */
export type ProductLine = { id: string; line?: number; name: string; share: number };
/** 产线分配 of one good in a works: its products and shares; for the operator, the other products (same design: join at once; else retool). */
export type AllocationRow = { good: string; goodName: string; unit: string; parts: (ProductRow & ReadyState & { share: number })[];
  options?: (ProductRow & ReadyState & { sameDesign: boolean; goodName?: string; quote?: Quote })[] };
/** 一条产线的产品分配（line = 线号）；别的商品的产品也列在 options 里，要换产。 */
export type LineAllocation = AllocationRow & { line: number };
/** One good the company can design products of: its components and the known techs of each. */
/** A vehicle design's figures: room by cargo class, speed, crew, life, fuel; a locomotive's haul and train length. */
export type VehicleFigures = { payload: number; room?: { class: string; tons: number; seats?: number }[]; kmPerDay: number; crew: number; lifeYears: number; haulTons?: number; maxCars?: number;
  fuel: { good: string; tonsPerDay: number }[] };
export type DesignOption = { id: string; name: string; effects: string; known: boolean; default: boolean; cost?: number };
/** One component of a design; a list slot (a train's wagons) takes several kinds in pieces, min..max in all. */
export type DesignComponent = { id: string; name: string; list?: boolean; min?: number; max?: number; defaultCount?: number; techs: DesignOption[] };
export type DesignGood = { id: string; name: string; unit: string; branded: boolean; vehicle?: boolean;
  /** 产品形态（ProductForms.md）：stages 的商品一道道选工序（Processes.md 第 9 节）。 */
  form?: string; stages?: StageSlot[];
  bases: { id: string; name: string; techs: string[]; counts?: Record<string, number>; brandId: string }[]; components: DesignComponent[] };
export type DesignConstraint = { name: string; measure: string; limit: string; used: number; max: number; tons: boolean };
export type DesignQuote = { base: string; same: string; sameName?: string; similarity: number; price: number; techs: string[]; attributes: { id: string; name: string; value: number; base: number }[];
  counts?: Record<string, number>; text?: string; worth?: number; vehicle?: VehicleFigures & { constraints: DesignConstraint[] };
  materials?: { good: string; unit: string; perUnit: number }[]; work?: number; chain?: StageChain };
export type BrandRow = { id: string; name: string; products: { id: string; name: string }[] };
export type OrderView = { good: string; units: number; supplier: string; status: string; made: number; arrive?: string };
/** One payment of a building (按节点付款): its share of the works and what that costs (the last with the overrun, or its range
 *  before building); a railway's rails and sleepers with the middle one; once building, paid / next / later and when due. */
export type StageView = { name: string; share: number; works: number; overrun?: number; last?: boolean; overrunMin?: number; overrunMax?: number;
  status?: 'paid' | 'next' | 'later'; dueDate?: string;
  materials?: { cost: number; iron?: string; timber?: string; from?: string; to?: string; bought?: boolean } };
export type ProjectView = { id: string; name: string; kind: string; phase: string; start: string; phaseEnd?: string; progress?: number; paid: number; overrun: number; delay: number;
  /** The wait for money going on: its days (and all the project's), what it waits for, how much is due and the company's cash. */
  stalled: boolean; stalledDays: number; stalledTotal?: number; stalledFor?: string; due?: number; cash?: number; stages?: StageView[]; orders: OrderView[] };
/** A line's crew, rated daily output, wages and upkeep per day on its current shift. */
export type LineFigures = { required: number; nominal: number; wages: number; upkeep: number };
export type MethodOption = { id: string; name: string; effects: string; known: boolean; quote?: Quote; after?: LineFigures };
/** 扩建 = 新增一条产线：做哪种商品、哪个产品，线的规模（这类设施一级的几成）、单班额定日产、人手与报价。 */
export type ExpandChoice = ReadyState & { good: string; goodName: string; unit: string; size: number; nominal: number; crew: number; productId: string; product?: string; quote: Quote };
/** 工厂：新增产线的各个选择；服务设施：扩建一级（expandSize）。 */
export type FacilityOptions = { expand: ExpandChoice[]; expandSize?: Quote; canIdle: boolean };
export type Machine = { good: string; units: number; supplier: string; attributes: string };
export type Redesign = { id: string; name: string; component: string; effects: string; quote: Quote };
export type TechQuote = { ok: boolean; reason: string; cost: number; weeks: number; nearest: string; distance: number; premium: boolean };
export type TechRow = { id: string; name: string; class: string; component: string; effects: string; skills: string; note: string; inUse: boolean; public: boolean; known: boolean; discovered: boolean; source: string; disposal: string;
  pressure: number; research: number; reverse: number; poach: number; patent?: { holder: string; mine: boolean; expires: string }; quote?: TechQuote;
  canReverse?: boolean; reverseReason?: string; canPoach?: boolean; poachReason?: string; canPatent: boolean; canPublish: boolean; licensees?: { id: string; name: string }[]; publicFee?: number };
export type LicenceView = { id: string; tech: string; out: boolean; other: string; fee: number; royalty: number; status: string; pending: boolean; canAnswer: boolean; related: boolean; belowPublic: boolean; royaltiesPaid: number };
export type KnowledgeView = { research?: { tech: string; name: string; cost: number; paid: number; weeks: number; paidWeeks: number; progress: number; stalled: boolean; disposal: string }; techs: TechRow[]; licences: LicenceView[]; notices: { date: string; kind: string; tech: string; other: string }[] };
export type LoanView = { id: string; held: boolean; other: string; principal: number; rate: number; overdueWeeks: number; callArrears: number; lastInterest: number; lastCalled: number; opened: string; pledge?: string };
export type BankOption = { id: string; name: string; rate: number; limit: number; collateral: number; coverage: number; coverageKnown: boolean; available: number; lendable: number };
export type FinanceView = { isBank: boolean; loans: LoanView[]; arrears: { creditor: string; category: string; amount: number; since: string }[]; credit?: { profitAvg: number; lastOperating: number; weeks: number; distress: number; distressLimit: number; restructured: number };
  fixedWeekly: number; debt: number; banks?: BankOption[]; book?: { lent: number; repaid: number; capitalized: number; writtenOff: number; interest: number; lendable: number };
  pledgeable?: { entity: string; name: string; voting: number; nonVoting: number; shares: number; value: number; max: number }[]; notices: { date: string; kind: string; amount: number; other?: string }[]; canBorrow: boolean };
export type BuildOption = { companyId: string; company: string; type: string; typeName: string; quote: Quote; resource?: string };
/** 建筑：a facility type behind the 建筑 button, with the group's companies that build it (or why none can). */
export type BuildMenuRow = { type: string; typeName: string; business: string; businessName: string; resource?: string; interest: string;
  companies: { id: string; name: string; cash: number }[]; reason?: string };
export type BuildView = { type: string; typeName: string; company: string; companyName?: string; interest: string };

// ---- 运输运营：车队、服务、订购车船 ----
/** One group of vehicles: model, units, base, where it runs; targets: where the operator may send it. */
export type FleetView = { id: string; name: string; good: string; goodName: string; unit: string; units: number; baseId: string; base?: string; maker: string; design: string;
  line: string; market: string; assignment: string; running: boolean; ageYears: number; book: number;
  /** 耐用品：who keeps it up and at what a year (no one: unkept), its life used (age and the years lost to upkeep not done),
   *  the bills' period and the wear a day without upkeep counts for, the last bill of its owner with its keeper. */
  keeper?: string; unkept?: boolean; upkeepYear?: number; usedYears?: number; lostYears?: number; settleDays?: number; missedWear?: number;
  billDue?: number; billPaid?: number;
  targets?: { line?: string; market?: string; name: string }[]; noTargets?: string } & VehicleFigures;
/** One service a company runs (a line, or delivery in a 集散区): its fare (× the base) and last week's figures. */
export type ServiceView = { key: string; line: string; market: string; name: string; fare: number; basePerTon?: number; mode?: string; vehicles?: number; transit?: number; every?: number;
  room?: number; rivals?: number; tonsPerDay?: number; offered?: number; carried?: number; util?: number; revenue?: number; costs?: number;
  /** 远程客运：seats a departure (coaches), a ship's shared hold, the passenger fare (× the base) and base, days aboard; last week's passengers. */
  seats?: number; sharedHold?: boolean; paxFare?: number; paxBase?: number; paxDays?: number; passengers?: number; paxRevenue?: number; seatsOffered?: number; seatsTaken?: number;
  /** The cheaper class on the line (三等, steerage) and its share of the fare. */
  className?: string; classShare?: number;
  /** 同线别家：the other carriers on the line, as a shipper or traveller sees them. */
  rivalList?: RivalView[] };
/** Another carrier on a line: its vehicles, timetable, room and seats a departure, freight rate and fares (and × the line's
 *  base), last week's tons and passengers (companies only; outside carriers keep no figures). */
export type RivalView = { name: string; vehicleModels?: string; vehicles: number; every: number; transit: number; room: number; rate: number; rateX?: number;
  seats?: number; paxDays?: number; paxFare?: number; paxX?: number; className?: string; classFare?: number;
  carried?: number; offered?: number; passengers?: number; seatsTaken?: number; seatsOffered?: number };
/** A model the operator may order at a base: its figures, its landed price for one and its cost per thousand ton-km there. */
export type ModelView = VehicleFigures & { id: string; name: string; maker: string; design: string; durability: number; ok: boolean; reason?: string; price: number;
  landed?: number; days?: number; perKTonKm?: number };
export type TransportBase = { id: string; name: string; type: string; market: string;
  goods: { id: string; name: string; unit: string; outsideDesign: string; outside?: ModelView; models: ModelView[] }[] };
export type TransportView = { fleets: FleetView[]; services: ServiceView[]; bases: TransportBase[]; book: number; fixedWeekly: number };
/** 铁路段（Transport.md 第 21 节）：选线报价、自家的段、特许与修建、通行权。 */
export type RailPlanPlot = { id: string; name: string; km: number; terrain: string; build: number; land: number; earthworks: number };
export type RailPlan = { fromId: string; toId: string; from: string; to: string; km: number; difficulty: number; fee: number; land: number; earthworks: number; track: number;
  stations: number; materials: number; buildCost: number; total: number; iron: string; timber: string; approvalWeeks: number; buildWeeks: number; plots: RailPlanPlot[]; existing: string[];
  stages: StageView[] };
export type RailAccessRow = { id: string; line: string; lineName: string; holder: string; user: string; toll: number; tollPerTon?: number; status: string; statusName: string;
  tollsPaid: number; carried: number; since?: string; holderSide: boolean; canAnswer: boolean; canEnd: boolean };
export type RailSegment = { id: string; name: string; from: string; to: string; km: number; rate: number; book: number; upkeepYear: number; opened?: string;
  plots: { name: string; terrain: string; build: number }[]; trains: number; otherTrains: number; carried?: number; revenue?: number; users: RailAccessRow[] };
export type RailConcession = { id: string; fromId: string; toId: string; from: string; to: string; km: number; difficulty: number; status: string; statusName: string; fee: number; buildCost: number;
  materials: number; iron?: string; timber?: string; stages?: StageView[]; buildWeeks: number; applied: string; decision: string; expiry?: string; line?: string; canBuild: boolean;
  canAbandon: boolean; project?: ProjectView };
export type RailOpenLine = { line: string; name: string; holder: string; from: string; to: string; km: number; rate: number; ask: number; state?: string };
export type RailView = { canOperate: boolean; askPermille: number; minPermille: number; maxPermille: number; segments: RailSegment[]; concessions: RailConcession[];
  access: RailAccessRow[]; open: RailOpenLine[]; markets: { id: string; name: string; neighbours: { id: string; name: string }[] }[] };
export type Seizure = { id: string; entity: string; bank: string; borrower: string; shares: number; ask: number; deadline: string };

// ---- 经营界面：周史 ----
export type EntityWeek = { week: number; date: string; cash: number; income: number; spending: number; operating: number; investment: number; debt: number; book: number };
export type LineWeek = { week: number; date: string; produced: number[]; nominal: number[]; effective: number; heads: number; required: number; variable: number; fixed: number; unitCost?: number; limit: string; project: boolean };
export type ProductWeek = { week: number; date: string; price: number; units: number; revenue: number; unitCost?: number };
export type GroupWeek = { week: number; date: string; familyCash: number; wealth: number; power: number };
export type Injector = { id: string; name: string; cash: number; wholly: boolean };

export type Facility = { id: string; name: string; type: string; typeId?: string; size: number; companyId: string; company: string; groupId: string; group: string; mine: boolean; plotId?: string; plot: string; market: string; production?: LineView; lines?: LineCard[];
  method?: string; equipment?: Machine[]; idle?: boolean; installing?: boolean; project?: ProjectView; options?: FacilityOptions;
  status?: string; ratio?: number; spark?: number[] };
export type ShiftPreview = { id: string; name: string; current: boolean; required: number; nominal: number[]; wages: number; upkeep: number; hire: number; quality: number };
export type FacilityDetail = Facility & { canOperate: boolean; typeId: string; marketId: string; country: string; business: string; status: string; lon?: number; lat?: number;
  book: number; costBasis: number; depreciation: number; resourcePoint?: string; resourceLeft?: number; resourceDepletes?: boolean;
  /** 矿权：本公司在这处矿藏租下的块数、整处矿藏的剩余、平均每吨矿租。 */
  resourceBlocks?: number; resourceDeposit?: number; royaltyPerTon?: number; resourceId?: string; resourcePlot?: string;
  /** 调人：同一集散区、同一家公司的各条线（id = 线键）。 */
  transfer?: { id: string; name: string; heads: number; required: number }[];
  inputs?: { good: string; unit: string; outside: number; local: number; price?: number; source?: string; lastWeek?: InputBuy[] }[];
  warehouse?: { product: string; unit: string; quantity: number; cap: number }[];
  capacity?: { good: string; unit: string; perDay: number }[]; coverage?: { access: string; share: number }[]; depot?: (ProductRow & { quantity: number; origin: string; dock: boolean; spoilage?: number })[]; store?: DepotStore;
  fleets?: FleetView[]; baseGoods?: string[]; freightCoverage?: number; deliveryFare?: number;
  allocation?: AllocationRow[]; granary?: GranaryView; house?: HouseView;
  /** 工序形态的工厂：动力、传送（整座工厂一种），以及正在换动力或传送的工程。 */
  works?: WorksStage[]; worksProject?: ProjectView };
/** 粮仓（Agriculture.md 第 4 节）：tons; prices a ton. */
export type GranaryFarmer = { plot: string; name: string; ask: number; haul: number; net: number; stock: number; normal: number };
/** mode: 常平 (auto), 收粮, 存粮, 卖粮; limit: the owner's price (0: the market's); target: tons to buy up to or sell down to (−1: a
 *  full store, or an empty one); buy and sell: today's prices (0: not buying, or held); the year's normal price, its low after a
 *  harvest and high before one, the 常平 rule's prices; marketBuy: the least the farmers' grain costs at the gate now (whose);
 *  marketSell: the cheapest grain the works at the stores could buy elsewhere (whose). */
export type GranaryGrain = { crop: string; name: string; mode: 'auto' | 'buy' | 'hold' | 'sell'; limit: number; target: number; buy: number; sell: number;
  season: number; spoilage: number; held: number; annual: number; low: number; high: number; lowWhen: string; highWhen: string; ruleBuy: number; ruleSell: number;
  marketBuy: number; marketBuyFrom: string; marketSell: number; marketSellFrom: string;
  soldLastWeek: number; revenueLastWeek: number; boughtLastWeek: number; spoiledLastWeek: number; paidLastWeek: number;
  fromPlots: { name: string; tons: number }[]; farmers: GranaryFarmer[];
  /** 常平 today: '' (the seasons' rule), famine (平粜), short (a short harvest coming: kept), reserve (down to the reserve: kept);
   *  famineFrom: the price elsewhere that opens the reserve. */
  rule: '' | 'famine' | 'short' | 'reserve'; famineFrom: number;
  /** 年景：the next harvests here (days to the peak, negative while coming in), known once rolled; short: a short one is coming. */
  outlook: { date: string; days: number; in: boolean; known: boolean; yield: number; kind: string; event: string }[]; short: boolean };
/** step: how far over the market's price a granary buying at it offers. throughput: tons a day in and out at the shift; lastDay:
 *  what passed the gates the last day; reserve: 常平's share of the room kept in a normal year. */
export type GranaryView = { capacity: number; held: number; step: number; grains: GranaryGrain[];
  shift: 'half' | 'single' | 'double'; throughput: number; lastDay: number; porters: number; wagesPerWeek: number;
  shifts: { id: string; name: string; throughput: number; wagesPerWeek: number }[];
  reserve: number; reserveDefault: number; famineAt: number; famineDiscount: number; shortHarvest: number };

/** 集散仓（Trade.md 第 2 节）：a company's depots in a 集散区 together, in tons — room, what is on the shelves and still on the dock,
 *  装卸 a day at the shift, what passed the last day and the last week, the days of last week that closed with goods on the dock, and
 *  what spoiled; each shift's 装卸, porters and weekly wages and upkeep. */
export type DepotStore = { capacity: number; held: number; dock: number; size: number; shift: 'half' | 'single' | 'double'; throughput: number;
  lastDay: number; lastWeek: number; dockDays: number; spoiled: number;
  shifts: { id: 'half' | 'single' | 'double'; name: string; throughput: number; porters: number; wagesPerWeek: number; upkeepPerWeek: number }[];
  /** 出租：rented (this company's room here is rented from another); free tons to let and their cost a ton a month; the rentals here; the
   *  viewer's companies that could rent room here. */
  rented: boolean; free: number; costPerTonMonth: number; rentals: RentalRow[];
  renters: { id: string; name: string }[] };

/** 集散仓出租（Trade.md 第 15 节 3.5）：room let (owner) or rented (tenant), proposed, in force or lately ended. */
export type RentalRow = { id: string; role: 'owner' | 'tenant'; other: string; otherName: string; market: string; tons: number; months: number; rent: number;
  deposit: number; status: string; statusName: string; answerDays?: number; until?: string; paidMonths: number; faultDays?: number;
  canAnswer: boolean; canWithdraw: boolean };
/** 商号（Trade.md 第 17 节）：its company's papers in the 集散区, what its 账房 write a week (and a month) and keep at most, what its import
 *  lanes take a month; each lane: the product, where it is bought, how often, the sales expected a day, the order still loading, the day of
 *  the next, its profit a day. */
export type HouseLane = { product: string; name: string; maker: string; unit: string; from: string; every: number; forecast: number; open: number; next: string; profit: number };
export type HouseView = { papers: number; weekly: number; monthly: number; keep: number; monthlyUse: number; perOrder: number; documents: number;
  insuranceSea: number; insuranceInland: number; clerks: number; wagesPerWeek: number; upkeepPerWeek: number; lanes: HouseLane[] };
/** 提示栏：a kind of thing to know, and its items (a company to open, or a facility). */
export type AlertGroup = { kind: 'gate'; name: string; items: { text: string; company: string; facility?: string }[] };

/** 矿藏（Resources.md 第 8 节）：一块矿区，谁租着、到哪天。 */
export type DepositBlock = { index: number; left: number; status: string; lease?: string; companyId?: string; company?: string; mine?: boolean; statusName?: string; until?: string };
export type LeaseTerms = { regime: string; lessor: string; fee: number; rentPerYear: number; royaltyPerTon: number; termYears: number; weeks: number; merge: boolean };
export type DepositView = { id: string; name: string; kind: string; kindId: string; variety: string; attributes: { id: string; name: string; value: number; percent: boolean }[];
  difficulty: number; depletes: boolean; left: number; leased: boolean; good?: string; blockTons?: number; blocks?: DepositBlock[]; terms?: LeaseTerms; termsReason?: string;
  lessees?: { id: string; name: string; cash: number; ok: boolean; reason: string; block?: number }[] };
/** 矿权：公司面板的一行。 */
export type LeaseView = { id: string; point: string; pointName: string; plot: string; plotName: string; kind: string; block: number; blocks: number; status: string; statusName: string;
  regime: string; lessor: string; fee: number; rentPerYear: number; royaltyPerTon: number; merge: boolean; termYears: number; since: string; decision?: string; until?: string;
  left: number; raised: number; accrued: number; paid: number; lastDue: number; lastPaid: number; missed: number; canSurrender: boolean; canRenew: boolean; renewFrom?: string };
export type MiningView = { leases: LeaseView[]; weeklyRent: number; settleDays: number; forfeitMissed: number };

/** 文化（Agriculture.md）：a plot's people as they are now; staple is the name of the food they would rather eat. */
export type CultureView = { id: string; name: string; group: string; staple: string };
/** 粮价（Agriculture.md 第 6 节）：a grain of a 集散区 — its price at the hub, the season's normal price, the farmers' stores
 *  against a normal year's, and its weeks. */
export type GrainView = { crop: string; name: string; price: number; season: number; stores: number; normal: number; port: boolean;
  weeks: { week: number; price: number; season: number; stores: number }[] };
/** 农户（Agriculture.md）：one crop of a plot's farmers. Tons; prices a ton. */
export type CropView = { crop: string; name: string; zone: string; tonsPerYear: number; stock: number; normalStock: number; ask: number; normal: number;
  expected: number; soldLastWeek: number; eatenLastWeek: number; householdsLastWeek: number; harvestLastWeek: number; nextHarvestDays: number; harvests: string[];
  /** 年景：each harvest's current or next season; kind '' is a normal one; unknown until noticeDays before it. */
  seasons: { date: string; days: number; now: boolean; known: boolean; kind?: string; yield?: number; event?: string }[] };
export type PlotView = { id: string; market: string; facilities: Facility[]; resourcePoints: DepositView[]; demandPending: boolean; population?: number; populationSource?: string; culture?: CultureView; crops?: CropView[]; build?: BuildOption[] };
export type ProductRow = { productId: string; product: string; good: string; unit: string; brand: string; brandId?: string; makerId: string; maker: string; sellerId: string; seller: string; group: string; mine: boolean;
  /** 糙粮自碾：the farmers or a port's importers in a product's place (no dossier). */
  outside?: boolean };
export type Offer = ProductRow & { quantity: number; price: number; origin: string; service: boolean };
export type Share = ProductRow & { units: number; share: number };
/** rigid: the share of the need bought before anything flexible, at any price; shortage: rigid units nobody could pay for
 *  (缺粮, Demand.md 第 6 节); postponed: flexible units put off for want of money; outside: units bought from the farmers or a
 *  port's importers (自碾). */
export type NeedView = { id: string; name: string; essential: boolean; rigid: number; demand: number; fulfilled: number; noBuy: number; noAccess: number; unaffordable: number; unmaintainable: number; noSupply: number; postponed: number; shortage: number; outside?: number; spend: number; shares: Share[];
  travel?: TravelWeek };
/** 远程出行 of a 集散区 over a week: trips by how they went, the leading destinations, trips bumped for want of seats. */
export type TravelWeek = { trips: number; fares: number; bumped: number; modes: { mode: string; name: string; trips: number; share: number }[]; destinations: { id: string; name: string; trips: number }[];
  classes?: { name: string; trips: number }[] };
/** A round trip on offer: where, how (the main mode, the route out), from whom, both fares, days on the way, seats left (−1: coaches only). */
export type TripView = { destinationId: string; destination: string; mode: string; modeName: string; className?: string; carrier: string; price: number; days: number; seats: number; route: string };
export type MarketArchive = { id: string; hubPlot?: string; companies: { companyId: string; company: string; group: string; mine: boolean; facilities: string[] }[]; population?: number; offers?: Offer[]; warehouse?: (ProductRow & { quantity: number })[]; inbound?: (ProductRow & { quantity: number; from: string; arrive: string })[]; period?: string; needs?: NeedView[]; trips?: TripView[] ; grain?: GrainView[] };
/** 现价成本的一项：an input at its cheapest source now (delivered to the works), a unit of the product. */
export type CostPart = { good: string; unit: string; perUnit: number; price: number; source: string; cost: number };
/** 定价方式：固定价, or 按毛利率 (the price follows currentCost every day at marginTarget). currentCost = each input at its cheapest
 *  source now plus the line's work a unit (workPerUnit: yesterday's, or the recipe's before it has run). */
/** granary: a granary's grain, sold at the store's price (the granary's facility). */
export type Pricing = { marginPricing?: boolean; marginTarget?: number; currentCost?: number; workPerUnit?: number; costParts?: CostPart[]; granary?: string };
export type CompanyProduct = ProductRow & Pricing & { price: number; unitCost: number; useCost: number; lastWeekUnits: number; lastWeekRevenue: number; ai?: { date: string; move: number; expected: number; capacity: number };
  design?: { component: string; tech: string }[]; attributes?: string; vehicle?: string; project?: ProjectView; redesigns?: Redesign[]; history?: ProductWeek[];
  goodId?: string; branded?: boolean; lines?: ProductLine[];
  /** 工序形态：它的工序链（额定值）；经营者还有各道工序可选的做法，用来改型。 */
  chain?: StageChain; stages?: StageSlot[]; unit?: string } & ReadyState;
export type Flow = { category: string; amount: number };
export type GroupChoice = { id: string; name: string; description: string; home: string; familyCash: number; companies: string[]; mock: boolean };
export type ControlNode = { id: string; name: string; kind: string; business: string; stake: number; votes: number; controlled: boolean; cash: number; facilities: number; children: ControlNode[] };
export type CompanyCard = { id: string; name: string; kind: string; business: string; country: string; cash: number; book: number; debt: number; stake: number; votes: number; canOperate: boolean; canMajor: boolean;
  controller: string; controllerId: string; lastOperating?: number; profitAvg?: number; distress?: number; research?: string; projects: number; facilities: number; products: number;
  history: EntityWeek[]; works: { id: string; name: string; status: string; ratio: number }[]; net: number; placed: number; sharePrice: number; injectors: Injector[] };
export type GroupConsole = { id: string; name: string; description: string; familyTrust: string; familyCash: number; wealth: number; power: number; tree?: ControlNode; familyTrustId?: string; seizures?: Seizure[]; supply?: SupplyLinkRow[];
  history?: GroupWeek[]; companies?: CompanyCard[]; brands?: { id: string; name: string; companyId: string; company: string; products: ProductSummary[] }[]; founders?: { id: string; name: string; kind: string; cash: number; countryId: string }[]; businesses?: { id: string; name: string; types: string[] }[]; countries?: { id: string; name: string }[] };
export type Company = { id: string; name: string; kind: string; business: string; country: string; controller: string; groupId: string; group: string; cash: number; bookValue: number; canOperate: boolean; canMajor: boolean; canVeto: boolean; shareholders: { holder: string; group: string; share: number; votes: boolean }[]; facilities: Facility[];
  products?: CompanyProduct[]; canPrice?: boolean; aiRun?: boolean; shifts?: { id: string; name: string }[]; stock?: (ProductRow & { market: string; quantity: number; factory: boolean })[]; inTransit?: (ProductRow & { from: string; to: string; quantity: number; arrive: string })[]; income?: Flow[]; spending?: Flow[];
  knowledge?: KnowledgeView; projects?: ProjectView[]; projectHistory?: { name: string; kind: string; end: string; paid: number; cancelled: boolean }[]; finance?: FinanceView;
  history?: EntityWeek[]; net?: number; placed?: number; sharePrice?: number; injectors?: Injector[]; canDividend?: boolean; debt?: number;
  brands?: BrandRow[]; maxBrands?: number; maxProducts?: number; designable?: DesignGood[]; transport?: TransportView; rail?: RailView; mining?: MiningView };
export type TimeView = { date: string; day: number; paused: boolean; speed: number; halted: string; lastCommand?: { type: string; ok: boolean; message: string } };
export type Measure = 'share' | 'units' | 'revenue' | 'price' | 'stock' | 'awareness' | 'loyalty' | 'main' | 'delivered' | 'sellers';
export type FocusKind = 'group' | 'product' | 'brand' | 'company' | 'good';
export type ShareFocus = { kind: FocusKind; id: string; name?: string; measure?: Measure; measures?: Measure[]; goodId?: string; unit?: string; physical?: boolean;
  /** 选中商品 (goods) or the focused product; the products of a good to pick from. */
  selected?: string; selectedName?: string; products?: { id: string; name: string; company?: string; mine: boolean }[] };
export type GoodRef = { id: string; name: string; unit: string; physical: boolean };
/** One product a household on the plot can buy for a need: prices delivered to this plot, quality, standing, each tier's expected choice. */
export type PlotOffer = ProductRow & { outside?: boolean; goodId: string; service: boolean; perUnit: number; shelf: number; price: number; fee: number; delivered: number; useCost: number;
  /** 糙粮自碾：the households' own husking and pounding a need unit, inside the delivered price (not the farmers' ask). */
  work?: number;
  lead: number; stock: number; score: number; attributes: { id: string; name: string; value: number }[]; awareness: number; loyalty: number;
  weekUnits: number; weekShare: number; origin: string; shares: number[]; progress: number[]; main?: boolean; selected?: boolean;
  /** 愿付：each income group's buyers by group and the most each would pay (delivered) for it; price: the mean of those who look
   *  at more than the price (quality: their share). */
  wtp?: { price?: number; quality: number; groups: { name: string; share: number; price: number; quality: boolean }[] }[] };
/** groupNames: the need's buyer groups (Demand.md 第 5 节); tierGroups: each income group's affordability a and the groups' shares at it. */
export type PlotNeed = { id: string; name: string; unit: string; essential: boolean; wants: number[]; noBuy: number[]; unreached: number[]; weekUnits: number; offers: PlotOffer[];
  groupNames?: string[]; tierGroups?: { a: number; shares: number[] }[] };
/** 科技树（TechTree.md 第 7 节）：one tech as the company looking through the tree stands with it. */
export type TechNode = { id: string; name: string; class: 'method' | 'product' | 'machine' | 'science'; className: string; category: string; categoryName: string;
  industry: string; year: number; availableFrom: number; default: boolean; note: string; effects: string; equipment: string;
  known: boolean; public: boolean; discovered: boolean; inUse: boolean; relevant: boolean; pressure: number; progress: number; researching: boolean; stalled?: boolean;
  /** 超前（TechTree.md 第 1 节）：days still to its first year; this week's chance of discovering it (pressure × the ahead factor). */
  daysAhead: number; chance: number;
  disposal: string; source: string; requires: string[]; missing: string[];
  patent?: { holder: string; holderId: string; mine: boolean; ours: boolean; expires: string; claimedFrom?: string; relatedParty: boolean; claimants: { id: string; name: string }[] };
  /** ahead: what being ahead of its first year multiplies the cost and the weeks by (1 when it has come). */
  quote?: { ok: boolean; reason: string; cost: number; weeks: number; nearest: string; distance: number; premium: boolean; ahead: number };
  canPatent: boolean; canPublish: boolean; licensor?: string; licensorName?: string; licensees?: { id: string; name: string }[];
  links: { id: string; name: string; distance: number }[] };
/** A line of the tree: within a component (chain), an explicit link (another component or trade), a science's (gate). */
export type TechEdge = { from: string; to: string; fromName?: string; toName?: string; fromIndustry?: string; toIndustry?: string; distance: number; kind: 'chain' | 'link' | 'gate' };
export type TechTreeView = { company: string; companyName: string; year: number; operate: boolean; companies: { id: string; name: string }[];
  industries: { id: string; name: string; categories: { id: string; name: string; class: string }[] }[]; techs: TechNode[]; edges: TechEdge[] };
export type PlotMarket = { plot: string; name: string; market: string; marketName: string; country: string; reason?: string; population?: number; road?: number; localRate?: number;
  freightCoverage?: number; deliveryFare?: number; stations?: { company: string; group: string; tonsPerDay: number; fare: number; vehicles: number }[];
  coverage?: { name: string; share: number }[]; tiers?: { tier: number; name: string; share: number; income: number; dailyIncome: number }[]; needs?: PlotNeed[];
  /** The plot's 集散区 in full (its depot's goods, where its demand went and to whom, trips, goods on the way, warehouses). */
  marketArchive?: MarketArchive };
/** One product over every 集散区: last week's revenue, units, share of its needs, markets sold in. */
export type ProductSummary = ProductRow & ReadyState & { goodId: string; price: number; revenue: number; units: number; share: number; markets: number; brandId: string; history: ProductWeek[]; lines?: ProductLine[] };
/** A product or a brand in one 集散区 (share, price index, standing: negative = unknown). */
export type SubjectMarket = { id: string; name: string; country: string; lon: number; lat: number; share: number; units: number; revenue: number; needRevenue: number;
  priceIndex: number; averagePrice: number; stock: number; awareness: number; loyalty: number; rivals: { id: string; name: string; maker: string; share: number }[]; shelf?: number };
export type CompetitorRow = ProductRow & { self: boolean; units: number; revenue: number; share: number; averagePrice: number; listPrice: number; goodId: string; attributes: { id: string; value: number }[] };
export type ProductDossier = { kind: 'product' | 'brand'; id: string; name: string; brandId?: string; brand?: string; companyId: string; company: string; group: string; mine: boolean; canPrice: boolean;
  measures: Measure[]; products: (ProductSummary & Pricing & { groupViews?: { need: string; name: string; score: number; price: boolean }[]; unitCost: number; margin: number; attributes: { id: string; name: string; value: number }[]; design: string })[];
  markets: SubjectMarket[]; total: { revenue: number; units: number; stock: number; share: number; markets: number };
  competition: { key: string; name: string; revenue: number; rows: CompetitorRow[] }[]; shipments: number; inTransit: number; siblings?: { id: string; name: string }[] };
/** 焦点（HierarchyUI.md 第 4 节）：the node of the business tree in view, its map view, the path from the root, the views it offers.
 *  intel: what the player knows of the focus's company (100 for their own, HierarchyUI.md 第 3.1 节). */
export type TreeKind = 'group' | 'company' | 'brand' | 'product' | 'facility' | 'line' | 'good';
export type FocusView = { kind: TreeKind; id: string; view: string; root: string; observed: boolean; intel: number;
  path: { kind: TreeKind; id: string; name: string }[]; views: { id: string; name: string }[]; canBack?: boolean; canForward?: boolean };
/** 经营树：as much of a group as the observer's intel shows (fields below a company's level are absent). */
export type TreeProduct = { productId: string; product: string; goodId: string; good: string; unit: string; price: number; revenue?: number; units?: number; share?: number;
  markets?: number; brandId?: string; ready?: boolean; readyDate?: string };
export type TreeLine = { key: string; id: number; goodId: string; good: string; unit: string; produced?: number; nominal?: number; status?: string };
export type TreeFacility = { id: string; name: string; typeId: string; type: string; size?: number; status?: string; ratio?: number; lines: TreeLine[] };
export type TreeCompany = { id: string; name: string; business: string; emblem: string; hue: number; cash?: number; lastOperating?: number; intel: number; hiddenFields: string[];
  brands: { id: string; name: string; emblem: string; products: TreeProduct[] }[]; products: TreeProduct[]; facilities: TreeFacility[] };
export type BusinessTree = { id: string; name: string; mine: boolean; familyCash?: number; companies: TreeCompany[] };
/** 世界树：the countries with their 集散区, and the other groups (列强). */
export type WorldTreeView = { countries: { id: string; name: string; markets: (Market & { plots: number })[] }[]; powers: { id: string; name: string; home?: string }[] };
/** 世界树上所选地方的路径，及其集散区的地块. */
export type PlacePathView = { country?: { id: string; name: string }; market?: { id: string; name: string; plots: { id: string; name: string; population?: number }[] }; plot?: { id: string; name: string } };
/** 国家档案. */
export type CountryArchiveView = { id: string; name: string; population?: number; plots: number; cultures: { name: string; group: string; share: number }[];
  markets: { id: string; name: string; plots: number; population?: number; lon?: number; lat?: number }[]; buildings: number; mine: number;
  types: { typeId: string; type: string; count: number }[]; registered: number; groups: { groupId: string; group: string; mine: boolean; count: number; names: string }[] };
/** 商品书册（HierarchyUI.md 第 8.1 节）. */
export type GoodsIndexRow = { id: string; name: string; unit: string; category: string; output?: number; make: boolean; use: boolean };
export type GoodsIndexView = { goods: GoodsIndexRow[] };
export type ChainGood = { good: string; name: string; unit: string };
/** 商品档案. */
export type GoodDossierView = { id: string; name: string; unit: string; category: string; buyers: string; outside: boolean;
  /** 存储：whether it keeps in a store at all, its monthly spoilage where the climate is mild, and the hot-wet and cold multipliers. */
  stockable: boolean; spoilage: number; humid: number; cold: number;
  made: { type: string; typeId: string; inputs: (ChainGood & { perUnit: number })[]; equipment: (ChainGood & { count: number })[] }[];
  used: (ChainGood & { type: string; typeId: string; perUnit: number })[];
  equips: { type: string; typeId: string; count: number }[]; carries: { type: string; typeId: string }[]; stores: { type: string; typeId: string }[];
  needs: { id: string; name: string; perUnit: number }[];
  producers: { companyId: string; company: string; group?: string; mine: boolean; facilities: number; output?: number; share?: number }[];
  users: { companyId: string; company: string; group?: string; mine: boolean; facilities: number; role: string; bought?: number }[];
  markets: { market: string; name: string; lowest: number; average: number; stock: number; units: number; sellers: number; main?: string }[];
  figures: { output?: number; sold: number; average?: number; markets: number; producers: number; users: number } };
/** 集团内供需：one company makes what another of the group uses. */
export type SupplyLinkRow = { fromId: string; from: string; toId: string; to: string; good: string; goodName: string; unit: string; inside: number; outside: number };
export type World = { uiScale?: number; mapModes?: { id: string; name: string }[]; mapMetric?: MapMetric; selection?: Selection; hoverSelection?: Selection; cameraTier?: string; mapDomain?: 'province' | 'market' | 'country'; saveStatus?: string; mapMode: string; seaRoutes: boolean; routeView?: boolean; hover?: Geography; hoverRoute?: string; route?: SeaRoute; date: string; week: number; lon: number; lat: number; atmosphere: boolean; geography: Geography; markets: Market[]; time?: TimeView; scenario?: { name: string; description: string }; playerGroup?: string; groupChoices?: GroupChoice[]; group?: GroupConsole; company?: Company; plot?: PlotView; marketArchive?: MarketArchive; techTree?: TechTreeView;
  facility?: FacilityDetail; shareFocus?: ShareFocus; productDossier?: ProductDossier; flows?: { shown: boolean; paths: number; bought?: number; links?: number; good?: boolean; focus?: string };
  goods?: GoodRef[]; plotMarket?: PlotMarket; buildMenu?: BuildMenuRow[]; build?: BuildView; alerts?: AlertGroup[];
  focus?: FocusView; tree?: BusinessTree; worldTree?: WorldTreeView; placePath?: PlacePathView; countryArchive?: CountryArchiveView;
  goodsIndex?: GoodsIndexView; goodDossier?: GoodDossierView; clickSerial?: number; player?: { id: string; name: string; familyCash: number } };
