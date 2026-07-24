// Generate a NoC system with 2-D mesh topology and hardware memory interface.
//
// Topology:
// 1. Grid: power-of-two nodes on a rectangular grid (1:1 or 2:1 width:height).
//    IDs (1-based) assigned via a Hilbert curve (backward C base, first step east).
//    For 2:1 grids (w=2h), the curve is generated on a w×w square and the first w*h
//    points (top half) are taken. Every 16 consecutive IDs form a 4×4 cluster.
// 2. Links: 2-D mesh (no wrapping). Directions: 0=North, 1=East, 2=South, 3=West.
//    Boundary nodes have fewer links (no link in the direction off the edge).
// 3. MC zones: continuous segments of the Hilbert curve, one per MC.
//    Each MC's zone contains whole 16-node clusters.
// 4. MC connection: in each cluster, the node closest to the grid center is the
//    connection point. That node gets an extra eject port for MC-bound traffic.
// 5. Routing: standard XY routing for all traffic.
//    - PU->PU: XY route directly.
//    - PU->MC: XY route to the nearest MC connection node in that MC's zone.
// 6. Memory: MemIf per MC collects multi-flit requests, issues GlobalMemReq.
//    Responses route back via MemDistributor (per cluster) to individual PUs.
//    Cross-zone responses forwarded via inter-MemIf ring bus.

package koneko

import chisel3._
import chisel3.util._
import chisel3.experimental.hierarchy.{instantiable, public, Definition, Instance}

import koneko.bus._

case class SystemParameters(
  numMC: Int,
  numPU: Int,
  coreParams: CoreParameters,
) {
  require(isPow2(numMC) && isPow2(numPU))
  require(numPU >= numMC * 16)
  require(numMC >= 1)
  require(coreParams.memCtrlSizes.size == numMC)
}

// Topology computation — pure Scala, no hardware
object Topology {
  // Grid dimensions: 1:1 or 2:1
  def gridDims(numPU: Int): (Int, Int) = {
    val s = math.sqrt(numPU).toInt
    if (s * s == numPU) (s, s)
    else {
      val h = math.sqrt(numPU / 2).toInt
      require(2 * h * h == numPU, s"Cannot form 1:1 or 2:1 grid for numPU=$numPU")
      (2 * h, h)
    }
  }

  // Hilbert curve: backward C base case (first step east).
  // H_1: (0,0)->(0,1)->(1,1)->(1,0). First half of any n×n curve covers top n/2 rows.
  // For w=2h grids, generate w×w square and take first w*h points (= top half).
  private def hilbertD2xy(n: Int, d: Int): (Int, Int) = {
    var rx = 0; var ry = 0; var s = 1; var t = d
    var x = 0; var y = 0
    while (s < n) {
      rx = if ((t & 2) != 0) 1 else 0
      ry = if (((t ^ rx) & 1) != 0) 1 else 0
      if (ry == 0) {
        if (rx == 1) { x = s - 1 - x; y = s - 1 - y }
        val tmp = x; x = y; y = tmp
      }
      x += s * rx
      y += s * ry
      t >>= 2
      s <<= 1
    }
    (x, y) // x→row, y→col
  }

  def hilbert(w: Int, h: Int): Seq[(Int, Int)] = {
    require(isPow2(w) && w >= 2)
    require(w == h || w == 2 * h)
    val square = (0 until w * w).map(d => hilbertD2xy(w, d))
    val curve = square.take(w * h)

    // Harness: verify coverage and adjacency
    for (row <- 0 until h; col <- 0 until w)
      require(curve.contains((row, col)), s"Hilbert curve missing position ($row, $col)")
    for (idx <- 1 until curve.length) {
      val (r1, c1) = curve(idx - 1)
      val (r2, c2) = curve(idx)
      require(math.abs(r2 - r1) + math.abs(c2 - c1) == 1,
        s"Hilbert curve non-adjacent at index ${idx - 1}→$idx: ($r1,$c1)→($r2,$c2)")
    }
    curve
  }

  // Link directions: 0=North, 1=East, 2=South, 3=West
  val NORTH = 0; val EAST = 1; val SOUTH = 2; val WEST = 3
  def opposite(dir: Int): Int = (dir + 2) % 4

  // Mesh neighbor (no wrapping). Returns None if at boundary.
  def meshNeighbor(row: Int, col: Int, dir: Int, gridW: Int, gridH: Int): Option[(Int, Int)] = dir match {
    case NORTH => if (row > 0) Some((row - 1, col)) else None
    case EAST  => if (col < gridW - 1) Some((row, col + 1)) else None
    case SOUTH => if (row < gridH - 1) Some((row + 1, col)) else None
    case WEST  => if (col > 0) Some((row, col - 1)) else None
    case _     => None
  }

  // Standard XY routing on mesh: X (column) first, then Y (row). No wrap.
  def xyRouteDir(sr: Int, sc: Int, dr: Int, dc: Int): Int = {
    if (sc != dc) { if (dc > sc) EAST else WEST }
    else { if (dr > sr) SOUTH else NORTH }
  }

  // Manhattan distance on mesh (no wrap)
  def meshDist(r1: Int, c1: Int, r2: Int, c2: Int): Int =
    math.abs(r2 - r1) + math.abs(c2 - c1)

  case class TopoInfo(
    gridW: Int,
    gridH: Int,
    // (row, col) position of each PU by ID (1-based)
    puPos: Map[Int, (Int, Int)],
    // Inverse: (row, col) -> PU ID
    posToId: Map[(Int, Int), Int],
    puIds: Seq[Int],
    mcIds: Seq[Int], // 0x8001, 0x8002, ...
    // MC ID -> cluster indices assigned to this MC
    mcZones: Map[Int, Seq[Int]],
    // MC ID -> list of connection PU IDs (one per cluster in the zone)
    mcConns: Map[Int, Seq[Int]],
    // PU ID -> which MC(s) this PU has an eject for (PU ID -> list of MC IDs)
    puMcEjects: Map[Int, Seq[Int]],
    // Per-PU: number of mesh directions that have neighbors (determines ingress/egress count)
    puMeshDirs: Map[Int, Seq[Int]], // PU ID -> list of active directions
    // Per-PU forwarding table (dst -> egress link index)
    puTables: Map[Int, Map[Int, Int]],
    // Per-PU locals list
    puLocals: Map[Int, Seq[Local]],
    // Total egress count per PU (always = active mesh directions)
    puNumEgress: Map[Int, Int],
    // Mapping from PU mesh direction to egress index
    puDirToEgress: Map[Int, Map[Int, Int]],
  )

  def build(numPU: Int, numMC: Int): TopoInfo = {
    val (gridW, gridH) = gridDims(numPU)
    val numClusters = numPU / 16

    // 1. Generate Hilbert curve and assign IDs (1-based)
    val curve = hilbert(gridW, gridH)
    require(curve.length == numPU, s"Hilbert curve length ${curve.length} != numPU $numPU")
    // Verify no duplicates
    require(curve.toSet.size == numPU, "Hilbert curve has duplicate positions")

    val puIds = (1 to numPU)
    val puPos: Map[Int, (Int, Int)] = puIds.zip(curve).toMap
    val posToId: Map[(Int, Int), Int] = curve.zip(puIds).toMap

    // 2. Cluster assignment: every 16 consecutive Hilbert IDs form a cluster
    val clusters = (0 until numClusters).map { ci =>
      (ci, (ci * 16 + 1 to ci * 16 + 16))
    }

    // 3. MC zone assignment: split the cluster sequence into numMC continuous segments
    // Recursive bisection of cluster indices [0, numClusters)
    val mcIds = (0 until numMC).map(_ + 0x8001)
    val clustersPerMC = numClusters / numMC
    val mcZones: Map[Int, Seq[Int]] = mcIds.zipWithIndex.map { case (mcId, mi) =>
      mcId -> (mi * clustersPerMC until (mi + 1) * clustersPerMC)
    }.toMap

    // 4. Connection nodes: the node in each cluster closest to the grid center
    val mcConns: Map[Int, Seq[Int]] = mcZones.map { case (mcId, clusterIdxs) =>
      mcId -> clusterIdxs.map { ci =>
        val clusterNodes = ci * 16 + 1 to ci * 16 + 16
        clusterNodes.minBy { puId =>
          val (r, c) = puPos(puId)
          math.abs(r - gridH / 2.0) + math.abs(c - gridW / 2.0)
        }
      }
    }

    // PU -> list of MCs it has eject ports for
    val puMcEjects: Map[Int, Seq[Int]] = {
      val m = scala.collection.mutable.Map[Int, List[Int]]().withDefaultValue(Nil)
      for ((mcId, conns) <- mcConns; puId <- conns) {
        m(puId) = m(puId) :+ mcId
      }
      m.toMap
    }

    // 5. Mesh: for each PU, determine which of N/E/S/W neighbors exist
    val puMeshDirs: Map[Int, Seq[Int]] = puIds.map { id =>
      val (r, c) = puPos(id)
      val dirs = (0 until 4).filter(d => meshNeighbor(r, c, d, gridW, gridH).isDefined)
      id -> dirs
    }.toMap

    // Egress count = number of active mesh directions
    val puNumEgress: Map[Int, Int] = puMeshDirs.map { case (id, dirs) => id -> dirs.length }

    // Map mesh direction -> egress index (dense packing)
    val puDirToEgress: Map[Int, Map[Int, Int]] = puMeshDirs.map { case (id, dirs) =>
      id -> dirs.zipWithIndex.toMap
    }

    // Inverse: egress index -> mesh direction
    val puEgressToDir: Map[Int, Map[Int, Int]] = puDirToEgress.map { case (id, m) =>
      id -> m.map(_.swap)
    }

    // Per-PU ingress also = number of active mesh directions (symmetric mesh)
    // Ingress index i corresponds to the same direction as egress index i
    // But ingress i carries traffic from the opposite direction's neighbor
    // Actually, for mesh connection, ingress and egress are paired per direction:
    //   egress(i) sends in direction dirs(i), ingress(i) receives from direction dirs(i)
    //   When connecting, A.egress(i) [going East] connects to B.ingress(j) [from West]

    // 6. Build forwarding tables
    val puTables: Map[Int, Map[Int, Int]] = puIds.map { srcId =>
      val (sr, sc) = puPos(srcId)
      val dirMap = puDirToEgress(srcId)
      val table = scala.collection.mutable.Map[Int, Int]()

      // Routes to all other PUs via XY routing
      for (dstId <- puIds if dstId != srcId) {
        val (dr, dc) = puPos(dstId)
        val dir = xyRouteDir(sr, sc, dr, dc)
        table(dstId) = dirMap(dir)
      }

      // Routes to MCs: XY to the nearest connection node in the MC's zone
      for (mcId <- mcIds) {
        val conns = mcConns(mcId)
        if (puMcEjects.getOrElse(srcId, Nil).contains(mcId)) {
          // This PU has a direct MC eject — handled by local eject, not forwarding table
        } else {
          // XY route toward nearest connection node
          val nearest = conns.minBy { connId =>
            val (cr, cc) = puPos(connId)
            meshDist(sr, sc, cr, cc)
          }
          val (cr, cc) = puPos(nearest)
          val dir = xyRouteDir(sr, sc, cr, cc)
          table(mcId) = dirMap(dir)
        }
      }

      // Route to peripheral (0x8000): XY toward PU1
      val periphId = 0x8000
      val periphPU = 1
      if (srcId == periphPU) {
        // PU1 has a local eject for 0x8800, no forwarding entry
      } else {
        val (pr, pc) = puPos(periphPU)
        val dir = xyRouteDir(sr, sc, pr, pc)
        table(periphId) = dirMap(dir)
      }

      srcId -> table.toMap
    }.toMap

    // 7. Build locals for each PU router
    //   - Local(id, inject=true, eject=true) for the core itself
    //   - Local(mcId, inject=false, eject=true) for each MC this PU connects to
    //   - Local(0x8000, inject=false, eject=true) on PU1 for peripheral
    val puLocals: Map[Int, Seq[Local]] = puIds.map { id =>
      val coreLocal = Local(id, inject = true, eject = true)
      val mcLocals = puMcEjects.getOrElse(id, Nil).map { mcId =>
        Local(mcId, inject = false, eject = true)
      }
      val periphLocal = if (id == 1) Seq(Local(0x8000, inject = false, eject = true)) else Nil
      id -> (Seq(coreLocal) ++ mcLocals ++ periphLocal)
    }.toMap

    TopoInfo(gridW, gridH, puPos, posToId, puIds, mcIds, mcZones, mcConns,
      puMcEjects, puMeshDirs, puTables, puLocals, puNumEgress, puDirToEgress)
  }

  private def isPow2(n: Int): Boolean = n > 0 && (n & (n - 1)) == 0
}

class System(implicit val params: SystemParameters) extends Module {
  implicit val coreParams: CoreParameters = params.coreParams
  implicit val busParams: MemBusParameters = MemBusParameters(coreParams)

  val topo = Topology.build(params.numPU, params.numMC)
  val flitType = new bus.Flit

  val numClusters = params.numPU / 16
  val clustersPerMC = numClusters / params.numMC

  val io = IO(new Bundle {
    val mem = Vec(params.numMC, new Bundle {
      val req  = Decoupled(new GlobalMemReq)
      val resp = Flipped(Valid(new GlobalMemResp))
    })
    val periph = new Bundle {
      val req  = Decoupled(new GlobalMemReq)
      val resp = Flipped(Valid(new GlobalMemResp))
    }
  })

  // --- Instantiate cores ---
  val coreDef = Definition(new Core)
  val cores = topo.puIds.map { id =>
    val inst = Instance(coreDef)
    inst.suggestName(s"pu_$id")
    inst.cfg.hartid := id.U
    (id, inst)
  }.toMap

  // --- Instantiate PU routers ---
  val puRouters = topo.puIds.map { id =>
    val numE = topo.puNumEgress(id)
    val numI = numE
    val locals = topo.puLocals(id)
    val table = topo.puTables(id)
    val router = Module(new Router(flitType, locals, numI, numE, 4, 8, table))
    router.suggestName(s"router_$id")
    (id, router)
  }.toMap

  // --- Connect mesh links ---
  val connectedPairs = scala.collection.mutable.Set[(Int, Int)]()
  for (id <- topo.puIds) {
    val (row, col) = topo.puPos(id)
    val dirs = topo.puMeshDirs(id)
    val dirToEgress = topo.puDirToEgress(id)

    for (dir <- dirs) {
      val (nr, nc) = Topology.meshNeighbor(row, col, dir, topo.gridW, topo.gridH).get
      val nid = topo.posToId((nr, nc))
      val oppDir = Topology.opposite(dir)
      val nDirToEgress = topo.puDirToEgress(nid)

      val pair = if (id < nid) (id, nid) else (nid, id)
      if (!connectedPairs.contains(pair)) {
        val eIdx = dirToEgress(dir)
        val nEIdx = nDirToEgress(oppDir)
        puRouters(nid).ingress(nEIdx) <> puRouters(id).egress(eIdx)
        puRouters(id).ingress(eIdx) <> puRouters(nid).egress(nEIdx)
        connectedPairs += pair
      }
    }
  }

  // --- Connect cores to PU routers (local 0 = core inject/eject) ---
  for (id <- topo.puIds) {
    val core = cores(id)
    val router = puRouters(id)
    val inject = router.injects(0).get
    val eject = router.ejects(0).get

    inject.valid := core.ext.out.valid
    inject.bits.src := id.U
    inject.bits.dst := core.ext.out.bits.dst
    for (i <- 0 until 4) inject.bits.data(i) := core.ext.out.bits.data(i)
    inject.bits.tag := core.ext.out.bits.tag
    core.ext.out.ready := inject.ready

    core.ext.in.valid := eject.valid
    core.ext.in.bits.src := eject.bits.src
    for (i <- 0 until 4) core.ext.in.bits.data(i) := eject.bits.data(i)
    core.ext.in.bits.tag := eject.bits.tag
    eject.ready := core.ext.in.ready
  }

  // --- Instantiate MemIfs (one per MC) ---
  val cumSizes = coreParams.memCtrlSizes.scanLeft(BigInt(0))(_ + _)
  val memIfs = topo.mcIds.zipWithIndex.map { case (mcId, mcIdx) =>
    val puStart = mcIdx * clustersPerMC * 16 + 1
    val puEnd   = (mcIdx + 1) * clustersPerMC * 16
    val memIf = Module(new DRAMIf(mcIdx, puStart, puEnd, clustersPerMC, 64, 16))
    memIf.suggestName(s"memif_${mcIdx + 1}")
    io.mem(mcIdx).req <> memIf.mem.req
    memIf.mem.resp := io.mem(mcIdx).resp
    (mcIdx, memIf)
  }.toMap

  // --- Instantiate peripheral MemIf (no zone-of-influence, all responses go via ring) ---
  // Config ROM at local addresses 0x28000000-0x28001000 (software: 0x68000000-0x68001000)
  //   0x28000000: numPU (word 0)
  //   0x28000008: numMC (word 2)
  //   0x28000010: PUs per MC (word 4)
  //   0x28000018: log2(memory line size in bytes) (word 6)
  //   0x28000020: each MC's memory size (64-bit, little-endian)
  val pusPerMC = params.numPU / params.numMC
  val mcSize = coreParams.memCtrlSizes.head // all MCs have the same size
  val configLine = BigInt(params.numPU) |
    (BigInt(params.numMC) << 64) |
    (BigInt(pusPerMC) << 128) |
    (BigInt(busParams.lineAddrShift) << 192) |
    (mcSize << 256)

  val configROM = Map(
    BigInt("28000000", 16) -> configLine,
  )
  val periphMemIf = Module(new PeripheralIf(16, configROM = configROM))
  periphMemIf.suggestName("memif_0")
  io.periph.req <> periphMemIf.mem.req
  periphMemIf.mem.resp := io.periph.resp

  // --- Connect MC eject ports to MemIf req inputs ---
  for ((mcId, mcIdx) <- topo.mcIds.zipWithIndex) {
    val connPUs = topo.mcConns(mcId) // one connection PU per cluster in zone
    for ((puId, ci) <- connPUs.zipWithIndex) {
      val locals = topo.puLocals(puId)
      val mcLocalIdx = locals.indexWhere(_.id == mcId)
      val ejectPort = puRouters(puId).ejects(mcLocalIdx).get

      memIfs(mcIdx).req(ci).valid    := ejectPort.valid
      memIfs(mcIdx).req(ci).bits.src := ejectPort.bits.src
      memIfs(mcIdx).req(ci).bits.dst := ejectPort.bits.dst
      memIfs(mcIdx).req(ci).bits.data := ejectPort.bits.data
      memIfs(mcIdx).req(ci).bits.tag := ejectPort.bits.tag
      ejectPort.ready := memIfs(mcIdx).req(ci).ready
    }
  }

  // --- Connect peripheral eject port on PU1 to peripheral MemIf ---
  {
    val locals = topo.puLocals(1)
    val periphLocalIdx = locals.indexWhere(_.id == 0x8000)
    val ejectPort = puRouters(1).ejects(periphLocalIdx).get

    periphMemIf.req(0).valid    := ejectPort.valid
    periphMemIf.req(0).bits.src := ejectPort.bits.src
    periphMemIf.req(0).bits.dst := ejectPort.bits.dst
    periphMemIf.req(0).bits.data := ejectPort.bits.data
    periphMemIf.req(0).bits.tag := ejectPort.bits.tag
    ejectPort.ready := periphMemIf.req(0).ready
  }

  // --- Ring bus: MC[0] -> MC[1] -> ... -> MC[last] -> periph -> MC[0] ---
  if (params.numMC == 1) {
    memIfs(0).ringOut <> periphMemIf.ringIn
    periphMemIf.ringOut <> memIfs(0).ringIn
  } else {
    // MC[i].ringOut -> MC[i+1].ringIn for i = 0..numMC-2
    for (i <- 0 until params.numMC - 1) {
      memIfs(i + 1).ringIn <> memIfs(i).ringOut
    }
    // MC[last].ringOut -> periph.ringIn
    periphMemIf.ringIn <> memIfs(params.numMC - 1).ringOut
    // periph.ringOut -> MC[0].ringIn
    memIfs(0).ringIn <> periphMemIf.ringOut
  }

  // --- Instantiate MemDistributors (one per cluster) and connect ---
  for (ci <- 0 until numClusters) {
    val mcIdx = ci / clustersPerMC
    val ciLocal = ci % clustersPerMC // cluster index within the MC's zone
    val puStart = ci * 16 + 1

    val dist = Module(new Distributor(puStart))
    dist.suggestName(s"distrib_${ci + 1}")

    // MemIf resp -> distributor input
    dist.in.unicast := memIfs(mcIdx).unicast(ciLocal)
    dist.in.broadcast <> memIfs(mcIdx).broadcast(ciLocal)

    // Distributor outputs -> core mem ports
    val readies = Wire(Vec(16, Bool()))
    for (j <- 0 until 16) {
      val puId = puStart + j
      cores(puId).mem.unicast.valid := dist.out.unicast.valids(j)
      cores(puId).mem.unicast.bits := dist.out.unicast.resp
      cores(puId).mem.broadcast.valid := dist.out.broadcast.valids(j)
      cores(puId).mem.broadcast.bits := dist.out.broadcast.resp
      readies(j) := cores(puId).mem.broadcast.ready
    }
    dist.out.broadcast.readies := readies.asUInt
  }
}
