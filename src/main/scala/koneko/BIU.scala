package koneko

import chisel3._
import chisel3.util._

import koneko._

class EvQueue(implicit val param: CoreParameters) extends Module {
  val alignment = IO(Input(UInt(5.W)))
  val enq = IO(Flipped(Decoupled(UInt(32.W))))
  val deq = IO(Decoupled(Vec(4, UInt(32.W))))
  // TODO: arbitrary alignment

  // TODO: configurable queue length
  val ram = Mem(32, Vec(4, UInt(32.W))) // TODO: calculate power as 16 lines
  val cnt = RegInit(0.U(5.W))

  val head = RegInit(0.U(6.W)) // 1 + log2up(32)
  val tail = RegInit(0.U(6.W)) // 1 + log2up(32)

  val empty = head === tail
  val full = head(4, 0) === tail(4, 0) && head(5) =/= tail(5)

  enq.ready := !full
  // TODO: support alignment > 4
  val last = cnt === alignment - 1.U
  when(enq.fire) {
    ram.write(tail(4, 0), VecInit(Seq.fill(4)(enq.bits)), UIntToOH(cnt(1, 0)).asBools)
    cnt := Mux(last, 0.U, cnt + 1.U)
    tail := Mux(last, tail + 1.U, tail)
  }

  val nhead = Mux(deq.fire, head + 1.U, head)
  head := nhead
  deq.valid := !empty
  deq.bits := RegNext(VecInit(ram.read(nhead).zipWithIndex.map({
    case (o, i) => Mux(
      nhead(4, 0) === tail(4,0) && i.U === cnt && enq.fire,
      enq.bits, o
    )
  })))
}

class BIU(implicit val param: CoreParameters) extends Module {
  /*
  val ifmem = IO(new Bundle {
    val req = Flipped(Decoupled(new MemReq))
    val resp = Valid(new MemResp)
  })

  val lsumem = IO(new Bundle {
    val req = Flipped(Decoupled(new MemReq))
    val resp = Valid(new MemResp)
  })
  */

  val msg = IO(Flipped(Decoupled(new Bundle {
    val reg = Vec(2, UInt(32.W))
    val enq = UInt(2.W) // 0, 1, 2
    val send = Bool()
    val target = UInt(16.W)
    val tag = UInt(16.W)
  })))

  val ext = IO(new Bundle {
    // TODO: last
    val out = Decoupled(new Bundle {
      val dst = UInt(16.W)
      val data = UInt(32.W)
      val tag = UInt(16.W)
    })

    val in = Flipped(Decoupled(new Bundle {
      val src = UInt(16.W)
      val data = UInt(32.W)
      val tag = UInt(16.W)
    }))
  })

  val br = IO(Decoupled(new Bundle {
    val regs = Vec(4, UInt(32.W))
    val target = UInt(32.W)
    val argcnt = UInt(5.W)
  }))

  val cfg = IO(Input(Vec(16, new Bundle {
    val handler = UInt(32.W)
    val argcnt = UInt(5.W)
  })))

  //////////////////////////
  // Sending
  //////////////////////////

  val sendingMsg = RegInit(false.B)
  val sendingTarget = RegEnable(msg.bits.target, msg.fire) // Actually the gate cond here can be optimized
  val sendingTag = RegEnable(msg.bits.tag, msg.fire)

  /*
  val sendingMem = RegInit(false.B)
  val sendingMemAddr = RegEnable(ifmem.req.bits.addr, ifmem.req.fire)
  val sendingMemTagSent = Reg(Bool())
  val sendingMemAddrSent = Reg(Bool())
   */

  // Register queues
  val queueEnqs = Seq.fill(2)(Wire(Decoupled(UInt(32.W))))
  val queues = queueEnqs.map(Queue(_, 16))
  val head = RegInit(0.U(1.W))
  val tail = RegInit(0.U(1.W))

  // Enq
  head := head + Mux(msg.fire, msg.bits.enq, 0.U)
  for((q, i) <- queueEnqs.zipWithIndex) {
    q.bits := Mux(i.U === head, msg.bits.reg(0), msg.bits.reg(1))
    q.valid := Mux(i.U === head, msg.bits.enq =/= 0.U, msg.bits.enq === 2.U) && msg.fire && !sendingMsg
    assert(q.ready) // TODO: actually handle queue full
  }

  val enqHasSpace = VecInit(queueEnqs.zipWithIndex.map({ case (q, i) => q.ready && i.U === head })).asUInt.orR

  // Deq
  val regDeq = Wire(Decoupled(UInt(32.W)))
  tail := tail + Mux(regDeq.fire, 1.U, 0.U)
  regDeq.bits := Mux1H(queues.zipWithIndex.map({ case (value, i) => (i.U === tail) -> value.bits }))
  regDeq.valid := sendingMsg && Mux1H(queues.zipWithIndex.map({ case (value, i) => (i.U === tail) -> value.valid }))
  for((q, i) <- queues.zipWithIndex) {
    q.ready := regDeq.ready && i.U === tail && sendingMsg
  }
  val regDrained = VecInit(queues.map(!_.valid)).asUInt.andR

  sendingMsg := MuxCase(sendingMsg, Seq(
    (sendingMsg && regDrained) -> false.B,
    (!sendingMsg && msg.fire && msg.bits.send) -> true.B
  ))
  // msg.ready := !sendingMsg && !sendingMem
  msg.ready := !sendingMsg && enqHasSpace

  // val memMapped = Decoupled(UInt(32.W))
  // memMapped.bits := Mux(sendingMem, sendingMemAddr, param.memTagBase.U)

  ext.out <> regDeq.map({ d => {
    val w = Wire(ext.out.bits.cloneType)
    w.dst := sendingTarget
    w.tag := sendingTag
    w.data := d
    w
  }})

  //////////////////////////
  // Recv & queues
  //////////////////////////
  val evqueues = Seq.fill(16)(Module(new EvQueue))
  for((e, i) <- evqueues.zipWithIndex) {
    e.alignment := cfg(i).argcnt
  }

  for ((e, i) <- evqueues.zipWithIndex) {
    e.enq.bits := ext.in.bits.data
    e.enq.valid := ext.in.valid && ext.in.bits.tag === i.U
  }
  ext.in.ready := VecInit(evqueues.zipWithIndex.map({ case (e, i) => e.enq.ready && ext.in.bits.tag === i.U })).asUInt.orR

  val evdeq = Module(new Arbiter(br.bits.cloneType, 16))
  val evdeqsMapped = evqueues.zipWithIndex.map({ case (e, i) => e.deq.map({ k => {
    val w = Wire(br.bits.cloneType)
    w.regs := k
    w.target := cfg(i).handler
    w.argcnt := cfg(i).argcnt
    w
  }})})

  for((i, j) <- evdeq.io.in.zip(evdeqsMapped)) i <> j
  br <> evdeq.io.out
}

// TODO: handles local send