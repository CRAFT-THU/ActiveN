// Generate a NoC of arbitrary topology

// The topology should be constructed in the following manner:
// 1. Lay down a power-of-two grid. The ID should be 1 ~ 2^n. The shape of the grid should be rectangular or (width:height = 2) rectangular.
//  - Later we'll need to cluster these nodes into 4x4 subgrid clusters. So the number should go as follows: The top left 4x4 subgrid gets ID  1~16, the subgrid to its right gets 17~32, and so on.
// 2. On top of this grid, first construct a 2-D torus links. The linkage should based on physical location.
// 3. Subpartition the curve base on the number of memory controllers, which shuould only contains whole 16-node subgrids (we require numMC * 16 <= numPU). After this, each memory controller will form its zone of influence, which is also a subgrid of power-of-two nodes.
// 4. Create a router for the MC (MC won't ever inject messages), ID 0x8000 + <MC_ID>. In each memory controller's zone of influence:
//   - If there is less or equal than 16 nodes, just connect the top-left node.
//   - If there are more than 16 nodes, further subpartition it into 16-node square chunks. Connect the top-left node of each chunk to the MC router.
// 5. Generate the routing table for each router. Right now let's use a simplified routing protocol:
//   - For traffics destined to a non-MC node, use XY routing.
//   - For traffics destined to a MC node, use XY routing to get to a connection node in the MC's zone of influence, then route to the MC router.
//   - MC -> MC traffic should never happen, chose any node.
// 6. Generate all the routers, generate all the cores
//  - Right now, ignore the memory response bus. Just expose them to the IO

package koneko

import chisel3._
import chisel3.util._
import chisel3.experimental.hierarchy.{instantiable, public, Definition, Instance}

import koneko.bus._

case class SystemParameters(
  numMC: Int, // Number of memory controllers
  numPU: Int, // Number of processing units (cores)
  coreParams: CoreParameters,
) {
  require(isPow2(numMC) && isPow2(numPU))
  require(numPU >= numMC * 16)
  require(numMC >= 1)
}

// NoC flit data type
class Flit extends Bundle with Routable {
  val src = UInt(16.W)
  val dst = UInt(16.W)
  val data = UInt(32.W)
  val tag = UInt(16.W)
  def prio = 0.U(1.W) // single VC for now
}

// Topology computation - pure Scala, no hardware
object Topology {
  // Compute grid dimensions for numPU nodes (width:height = 1:1 or 2:1)
  def gridDims(numPU: Int): (Int, Int) = {
    val s = math.sqrt(numPU).toInt
    if (s * s == numPU) (s, s)
    else {
      // 2:1 ratio: width = 2*height, width*height = numPU
      val h = math.sqrt(numPU / 2).toInt
      require(2 * h * h == numPU, s"Cannot form 1:1 or 2:1 grid for numPU=$numPU")
      (2 * h, h)
    }
  }

  // Convert (row, col) in the global grid to PU ID (1-based)
  // Clusters are 4x4, ordered raster-scan. Within cluster, row-major.
  def gridToId(row: Int, col: Int, gridW: Int): Int = {
    val clustersPerRow = gridW / 4
    val clusterRow = row / 4
    val clusterCol = col / 4
    val clusterIdx = clusterRow * clustersPerRow + clusterCol
    val localRow = row % 4
    val localCol = col % 4
    val localIdx = localRow * 4 + localCol
    clusterIdx * 16 + localIdx + 1 // 1-based
  }

  // Inverse: PU ID (1-based) to (row, col) in global grid
  def idToGrid(id: Int, gridW: Int): (Int, Int) = {
    val clustersPerRow = gridW / 4
    val clusterIdx = (id - 1) / 16
    val localIdx = (id - 1) % 16
    val clusterRow = clusterIdx / clustersPerRow
    val clusterCol = clusterIdx % clustersPerRow
    val localRow = localIdx / 4
    val localCol = localIdx % 4
    (clusterRow * 4 + localRow, clusterCol * 4 + localCol)
  }

  // Link directions: 0=North, 1=East, 2=South, 3=West
  val NORTH = 0; val EAST = 1; val SOUTH = 2; val WEST = 3
  def opposite(dir: Int): Int = (dir + 2) % 4

  // Torus neighbor: returns (neighborRow, neighborCol) with wrap-around
  def torusNeighbor(row: Int, col: Int, dir: Int, gridW: Int, gridH: Int): (Int, Int) = dir match {
    case NORTH => ((row - 1 + gridH) % gridH, col)
    case EAST  => (row, (col + 1) % gridW)
    case SOUTH => ((row + 1) % gridH, col)
    case WEST  => (row, (col - 1 + gridW) % gridW)
  }

  // Recursive bisection of a cluster grid region to assign MC zones.
  // region: (startClusterRow, startClusterCol, numClusterRows, numClusterCols)
  // mcIds: list of MC IDs to assign to this region
  // Returns: Map[MC_ID -> List[clusterIdx]] (raster-scan assignment)
  def bisectAssign(
    scr: Int, scc: Int, ncr: Int, ncc: Int,
    mcIds: Seq[Int], clustersPerRow: Int
  ): Map[Int, Seq[Int]] = {
    if (mcIds.length == 1) {
      // All clusters in this region belong to this MC
      val clusters = for (r <- scr until scr + ncr; c <- scc until scc + ncc) yield r * clustersPerRow + c
      Map(mcIds.head -> clusters)
    } else {
      val half = mcIds.length / 2
      val (left, right) = mcIds.splitAt(half)
      if (ncc >= ncr) {
        // Split vertically (columns)
        val halfCols = ncc / 2
        bisectAssign(scr, scc, ncr, halfCols, left, clustersPerRow) ++
          bisectAssign(scr, scc + halfCols, ncr, ncc - halfCols, right, clustersPerRow)
      } else {
        // Split horizontally (rows)
        val halfRows = ncr / 2
        bisectAssign(scr, scc, halfRows, ncc, left, clustersPerRow) ++
          bisectAssign(scr + halfRows, scc, ncr - halfRows, ncc, right, clustersPerRow)
      }
    }
  }

  // For an MC zone, find the connection nodes (top-left of each 4x4 chunk)
  // Zone is a set of cluster indices. Returns list of PU IDs that connect to the MC router.
  def mcConnectionNodes(clusterIndices: Seq[Int], gridW: Int): Seq[Int] = {
    // Each cluster is already a 4x4 = 16-node chunk. Top-left of each cluster.
    val clustersPerRow = gridW / 4
    clusterIndices.map { ci =>
      val cr = ci / clustersPerRow
      val cc = ci % clustersPerRow
      gridToId(cr * 4, cc * 4, gridW)
    }
  }

  // XY routing on a torus: from (sr,sc) to (dr,dc) on grid (W,H) with torus wrap
  // Returns: next direction to take
  def xyRouteDir(sr: Int, sc: Int, dr: Int, dc: Int, gridW: Int, gridH: Int): Int = {
    // X (column) first, then Y (row)
    if (sc != dc) {
      // Determine shortest path in column (torus)
      val diff = dc - sc
      val absDiff = math.abs(diff)
      if (absDiff <= gridW / 2) {
        if (diff > 0) EAST else WEST
      } else {
        if (diff > 0) WEST else EAST
      }
    } else {
      // Columns match, route in Y (row)
      val diff = dr - sr
      val absDiff = math.abs(diff)
      if (absDiff <= gridH / 2) {
        if (diff > 0) SOUTH else NORTH
      } else {
        if (diff > 0) NORTH else SOUTH
      }
    }
  }

  // Build a complete system topology description
  case class TopoInfo(
    gridW: Int,
    gridH: Int,
    puIds: Seq[Int],              // all PU IDs (1-based)
    mcIds: Seq[Int],              // 0x8000, 0x8001, ...
    mcZones: Map[Int, Seq[Int]],  // MC ID -> cluster indices
    mcConns: Map[Int, Seq[Int]],  // MC ID -> connection PU IDs
    puRouterNumLinks: Int,        // always 4 (torus) + optional MC link
    // PU routing tables (PU ID -> routing table Map[dst -> link])
    puTables: Map[Int, Map[Int, Int]],
    // MC routing tables
    mcTables: Map[Int, Map[Int, Int]],
    // MC link index on PU routers that connect to an MC
    puMcLinkMap: Map[Int, Int],   // PU ID -> which link index connects to MC
    // How many links each PU router has
    puLinkCounts: Map[Int, Int],
  )

  def build(numPU: Int, numMC: Int): TopoInfo = {
    val (gridW, gridH) = gridDims(numPU)
    val numClusters = numPU / 16
    val clustersPerRow = gridW / 4
    val clustersPerCol = gridH / 4

    // PU IDs
    val puIds = (1 to numPU)

    // MC IDs
    val mcIds = (0 until numMC).map(_ + 0x8000)

    // Assign clusters to MCs via recursive bisection
    val mcZones = bisectAssign(0, 0, clustersPerCol, clustersPerRow, 0 until numMC, clustersPerRow)
      .map { case (mcIdx, clusters) => (0x8000 + mcIdx, clusters) }

    // Connection nodes for each MC
    val mcConns = mcZones.map { case (mcId, clusters) =>
      (mcId, mcConnectionNodes(clusters, gridW))
    }

    // Which PU nodes are MC connection nodes, and which MC they connect to
    val puToMc: Map[Int, Int] = mcConns.flatMap { case (mcId, puIds) =>
      puIds.map(puId => (puId, mcId))
    }

    // PU link counts: 4 (torus) + 1 if MC connection node
    val puLinkCounts = puIds.map { id =>
      id -> (4 + (if (puToMc.contains(id)) 1 else 0))
    }.toMap

    // MC link index for connected PUs: link 4 (after N,E,S,W)
    val puMcLinkMap = puToMc.map { case (puId, _) => (puId, 4) }

    // Build PU routing tables
    val puTables = puIds.map { srcId =>
      val (sr, sc) = idToGrid(srcId, gridW)
      val table = scala.collection.mutable.Map[Int, Int]()

      // Routes to all other PUs via XY torus routing
      for (dstId <- puIds if dstId != srcId) {
        val (dr, dc) = idToGrid(dstId, gridW)
        table(dstId) = xyRouteDir(sr, sc, dr, dc, gridW, gridH)
      }

      // Routes to MCs: XY to the nearest connection node in the MC's zone,
      // then from there to the MC via the MC link
      for (mcId <- mcIds) {
        val connNodes = mcConns(mcId)
        if (puToMc.get(srcId).contains(mcId)) {
          // This PU is directly connected to this MC
          table(mcId) = puMcLinkMap(srcId) // link 4
        } else {
          // Find nearest connection node (shortest torus distance)
          val nearest = connNodes.minBy { connId =>
            val (cr, cc) = idToGrid(connId, gridW)
            torusDist(sr, sc, cr, cc, gridW, gridH)
          }
          val (cr, cc) = idToGrid(nearest, gridW)
          // Route toward that connection node using XY
          table(mcId) = xyRouteDir(sr, sc, cr, cc, gridW, gridH)
        }
      }

      srcId -> table.toMap
    }.toMap

    // Build MC routing tables: MC link i connects to connNodes(i)
    // MC routes to PUs by finding which connection node to forward to,
    // then using that link index
    val mcTables = mcIds.map { mcId =>
      val connNodes = mcConns(mcId)
      val table = scala.collection.mutable.Map[Int, Int]()

      for (dstId <- puIds) {
        // Find which connection node is closest to the destination
        val best = connNodes.zipWithIndex.minBy { case (connId, _) =>
          val (cr, cc) = idToGrid(connId, gridW)
          val (dr, dc) = idToGrid(dstId, gridW)
          torusDist(cr, cc, dr, dc, gridW, gridH)
        }
        table(dstId) = best._2 // link index in MC router
      }

      // MC->MC: pick any link (should never happen)
      for (otherMc <- mcIds if otherMc != mcId) {
        table(otherMc) = 0
      }

      mcId -> table.toMap
    }.toMap

    TopoInfo(gridW, gridH, puIds, mcIds, mcZones, mcConns,
      4, puTables, mcTables, puMcLinkMap, puLinkCounts)
  }

  def torusDist(r1: Int, c1: Int, r2: Int, c2: Int, w: Int, h: Int): Int = {
    val dx = { val d = math.abs(c2 - c1); math.min(d, w - d) }
    val dy = { val d = math.abs(r2 - r1); math.min(d, h - d) }
    dx + dy
  }
}

class System(implicit val params: SystemParameters) extends Module {
  implicit val coreParams: CoreParameters = params.coreParams

  val topo = Topology.build(params.numPU, params.numMC)
  val flitType = new Flit

  val io = IO(new Bundle {
    // Memory request flits ejected from MC routers -> external DRAM controllers
    val memOut = Vec(params.numMC, Decoupled(new Flit))
    // Memory responses back to each core (wide bus, bypasses NoC)
    val memResp = Vec(params.numPU, Flipped(Valid(new MemResp)))
  })

  // --- Instantiate cores ---
  val coreDef = Definition(new Core)
  val cores = topo.puIds.map { id =>
    val inst = Instance(coreDef)
    inst.cfg.hartid := id.U
    inst.mem := io.memResp(id - 1)
    (id, inst)
  }.toMap

  // --- Instantiate PU routers ---
  val puRouters = topo.puIds.map { id =>
    val nLinks = topo.puLinkCounts(id)
    val router = Module(new Router(flitType, id, nLinks, 1, 4, topo.puTables(id)))
    (id, router)
  }.toMap

  // --- Instantiate MC routers ---
  val mcRouters = topo.mcIds.map { mcId =>
    val nLinks = topo.mcConns(mcId).length
    val router = Module(new Router(flitType, mcId, nLinks, 1, 4, topo.mcTables(mcId)))
    (mcId, router)
  }.toMap

  // --- Connect PU routers: torus links (0=N, 1=E, 2=S, 3=W) ---
  // We only connect each pair once (e.g. A.egress(EAST) -> B.ingress and B.egress(WEST) -> A.ingress)
  val connectedPairs = scala.collection.mutable.Set[(Int, Int, Int)]() // (id, neighborId, dir)
  for (id <- topo.puIds) {
    val (row, col) = Topology.idToGrid(id, topo.gridW)
    for (dir <- 0 until 4) {
      val (nr, nc) = Topology.torusNeighbor(row, col, dir, topo.gridW, topo.gridH)
      val nid = Topology.gridToId(nr, nc, topo.gridW)
      val oppDir = Topology.opposite(dir)
      if (!connectedPairs.contains((nid, id, oppDir))) {
        // Connect: id.egress(dir) -> nid.ingress(oppDir)
        puRouters(nid).links(oppDir).ingress <> puRouters(id).links(dir).egress
        puRouters(id).links(dir).ingress <> puRouters(nid).links(oppDir).egress
        connectedPairs += ((id, nid, dir))
      }
    }
  }

  // --- Connect MC routers to PU routers ---
  for (mcId <- topo.mcIds) {
    val connNodes = topo.mcConns(mcId)
    for ((puId, mcLinkIdx) <- connNodes.zipWithIndex) {
      val puLinkIdx = topo.puMcLinkMap(puId) // link 4 on PU router
      // PU.egress(4) -> MC.ingress(mcLinkIdx) and MC.egress(mcLinkIdx) -> PU.ingress(4)
      mcRouters(mcId).links(mcLinkIdx).ingress <> puRouters(puId).links(puLinkIdx).egress
      puRouters(puId).links(puLinkIdx).ingress <> mcRouters(mcId).links(mcLinkIdx).egress
    }
  }

  // --- Connect cores to PU routers (inject/eject) ---
  for (id <- topo.puIds) {
    val core = cores(id)
    val router = puRouters(id)

    // Core ext.out -> router inject (add src field)
    router.inject.valid := core.ext.out.valid
    router.inject.bits.src := id.U
    router.inject.bits.dst := core.ext.out.bits.dst
    router.inject.bits.data := core.ext.out.bits.data
    router.inject.bits.tag := core.ext.out.bits.tag
    core.ext.out.ready := router.inject.ready

    // Router eject -> core ext.in
    core.ext.in.valid := router.eject.valid
    core.ext.in.bits.src := router.eject.bits.src
    core.ext.in.bits.data := router.eject.bits.data
    core.ext.in.bits.tag := router.eject.bits.tag
    router.eject.ready := core.ext.in.ready
  }

  // --- MC routers: no inject, eject goes to io.memOut ---
  for ((mcId, mcIdx) <- topo.mcIds.zipWithIndex) {
    val router = mcRouters(mcId)
    router.inject.valid := false.B
    router.inject.bits := DontCare
    io.memOut(mcIdx) <> router.eject
  }
}