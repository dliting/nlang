# stdlib public surface audit (0.13.8 D5.6)

Effective access of every top-level declaration in the stdlib
sources, recorded for the cross-module access-control rollout.
Effective access follows the language rule: a declaration with no
access prefix defaults to public; the compiler encodes public on
the wire regardless of spelling, so all entries below resolve to
FA_Public. No declaration in any stdlib module carries an explicit
`private`/`protected` prefix (verified mechanically on this walk).

Prelude files (`lang.n`, `containers.n`) are compile-time-inline
and excluded from the standard library's packages; they are audited
here for completeness but their members are not importable package
surface.

## concurrent.n

| Kind | Name | Effective access |
|------|------|------------------|
| class | Mutex | public |
| class | Semaphore | public |
| class | ReentrantMutex | public |
| class | AtomicInt | public |
| const class | ChannelClosedException | public |
| class | Channel | public |
| class | WaitGroup | public |

Verdict: all 7 top-level declarations public; nothing to hide.

## containers.n (prelude)

| Kind | Name | Effective access |
|------|------|------------------|
| class | List | public |
| class | Dict | public |

Verdict: all 2 top-level declarations public; nothing to hide.

## fs.n

| Kind | Name | Effective access |
|------|------|------------------|
| native function | exists | public |
| native function | isFile | public |
| native function | isDirectory | public |
| native function | size | public |
| native function | listFiles | public |
| native function | makeDirs | public |
| native function | remove | public |
| native function | join | public |
| native function | readBytes | public |
| native function | readFloats | public |
| native function | writeBytes | public |
| native function | modifiedTimeMs | public |

Verdict: all 12 top-level declarations public; nothing to hide.

## gzip.n

| Kind | Name | Effective access |
|------|------|------------------|
| const class | GzipException | public |
| native function | _gzipCompress | public |
| native function | _gzipDecompress | public |
| function | compress | public |
| function | decompress | public |

Verdict: all 5 top-level declarations public; nothing to hide.

## http.n

| Kind | Name | Effective access |
|------|------|------------------|
| const class | HttpException | public |
| class | HttpHeaders | public |
| class | HttpRequest | public |
| class | HttpResponse | public |
| class | HttpParser | public |
| class | HttpDate | public |
| class | HttpBodyReader | public |
| class | HttpConn | public |
| class | HttpServer | public |
| class | HttpPool | public |
| class | HttpClient | public |
| function | defaultClient | public |
| function | get | public |
| function | post | public |
| function | put | public |
| function | del | public |
| function | request | public |

Verdict: all 17 top-level declarations public; nothing to hide.

## io.n

| Kind | Name | Effective access |
|------|------|------------------|
| native function | print | public |
| native function | write | public |
| native function | eprint | public |
| native function | hasInput | public |
| native function | readLine | public |
| native function | readToken | public |
| native function | readChar | public |
| function | readInt | public |
| function | readLong | public |
| function | readFloat | public |
| function | readDouble | public |
| function | readBool | public |
| native function | readFile | public |
| native function | writeFile | public |
| native function | appendFile | public |
| class | ByteStream | public |
| class | FileStream | public |
| native function | parkForIo | public |
| native function | toBytes | public |
| native function | fromBytes | public |
| function | base64Encode | public |
| function | base64Decode | public |
| function | base64UrlEncode | public |
| function | base64UrlDecode | public |

Verdict: all 24 top-level declarations public; nothing to hide.

## lang.n (prelude)

| Kind | Name | Effective access |
|------|------|------------------|
| const class | Exception | public |
| const class | NullPointerException | public |
| const class | DivByZeroException | public |
| const class | IndexOutOfBoundsException | public |
| const class | AssertionException | public |
| const class | IOException | public |
| const class | InvalidCastException | public |
| const class | InitializationException | public |
| class | Object | public |
| class | String | public |
| class | Array | public |

Verdict: all 11 top-level declarations public; nothing to hide.

## math.n

| Kind | Name | Effective access |
|------|------|------------------|
| native function | sqrt | public |
| native function | sin | public |
| native function | cos | public |
| native function | tan | public |
| native function | asin | public |
| native function | acos | public |
| native function | atan | public |
| native function | atan2 | public |
| native function | pow | public |
| native function | exp | public |
| native function | log | public |
| native function | absi | public |
| native function | absf | public |
| native function | mini | public |
| native function | maxi | public |
| native function | minf | public |
| native function | maxf | public |
| native function | clampi | public |
| native function | clampf | public |
| native function | floor | public |
| native function | ceil | public |
| native function | round | public |
| native function | random | public |
| native function | srand | public |
| native function | randomi | public |
| native function | randomBytes | public |

Verdict: all 26 top-level declarations public; nothing to hide.

## net.n

| Kind | Name | Effective access |
|------|------|------------------|
| const class | SocketException | public |
| const class | TimeoutException | public |
| native function | _nowMs | public |
| native function | _lastErrorText | public |
| native function | _lastErrorCode | public |
| native function | _toBytes | public |
| native function | _fromBytes | public |
| function | _throwNet | public |
| native function | _tcpResolveStart | public |
| native function | _tcpResolveWait | public |
| native function | _tcpResolveEndpoint | public |
| native function | _tcpResolveRelease | public |
| function | resolveList | public |
| native function | _ping | public |
| native function | _icmpBind | public |
| native function | _icmpSendTo | public |
| native function | _icmpRecvFrom | public |
| native function | _icmpLastRecvAddress | public |
| native function | _icmpClose | public |
| function | ping | public |
| class | IcmpSocket | public |
| native function | _tcpBind | public |
| native function | _tcpLocalAddress | public |
| native function | _tcpLocalPort | public |
| native function | _tcpClose | public |
| native function | _tcpAccept | public |
| native function | _tcpConnectTry | public |
| native function | _tcpRead | public |
| native function | _tcpWrite | public |
| native function | _tcpShutdownWrite | public |
| native function | _tcpIsClosed | public |
| native function | _tcpPeerAddress | public |
| native function | _tcpPeerPort | public |
| native function | _tcpSetNoDelay | public |
| native function | _tcpSetKeepAlive | public |
| native function | _tcpSetSendBufferSize | public |
| native function | _tcpSetRecvBufferSize | public |
| class | TcpListener | public |
| class | TcpStream | public |
| native function | _udpBind | public |
| native function | _udpSendTo | public |
| native function | _udpConnect | public |
| native function | _udpSend | public |
| native function | _udpRecv | public |
| native function | _udpRecvFrom | public |
| native function | _udpLastRecvAddress | public |
| native function | _udpLastRecvPort | public |
| native function | _udpSetBroadcast | public |
| native function | _udpJoinMulticast | public |
| native function | _udpLeaveMulticast | public |
| native function | _udpSetMulticastTtl | public |
| native function | _udpSetTtl | public |
| native function | _udpSetSendBufferSize | public |
| native function | _udpSetRecvBufferSize | public |
| native function | _udpLocalAddress | public |
| native function | _udpLocalPort | public |
| native function | _udpPeerAddress | public |
| native function | _udpPeerPort | public |
| native function | _udpClose | public |
| class | UdpSocket | public |

Verdict: all 60 top-level declarations public; nothing to hide.

## regex.n

| Kind | Name | Effective access |
|------|------|------------------|
| const class | RegexException | public |
| function | quoteMeta | public |
| class | RENode | public |
| class | REBuilder | public |
| const class | REProg | public |
| class | REParser | public |

Verdict: all 6 top-level declarations public; nothing to hide.

## system.n

| Kind | Name | Effective access |
|------|------|------------------|
| native function | wallMs | public |
| native function | monoMs | public |
| const class | DateTimeException | public |
| const class | DateTime | public |

Verdict: all 4 top-level declarations public; nothing to hide.

## tensor.n

| Kind | Name | Effective access |
|------|------|------------------|
| function | full | public |
| function | full | public |
| function | full | public |
| function | full | public |
| function | full | public |
| function | full | public |
| function | full | public |
| function | full | public |
| function | arange | public |
| function | arange | public |
| function | arange | public |
| function | arange | public |
| native function | fullI | public |
| native function | fullL | public |
| native function | fullF | public |
| native function | fullD | public |
| native function | fullDimsI | public |
| native function | fullDimsL | public |
| native function | fullDimsF | public |
| native function | fullDimsD | public |
| native function | arangeI | public |
| native function | arangeL | public |
| native function | arangeF | public |
| native function | arangeD | public |
| native function | strideViewI | public |
| native function | viewSumI | public |
| native function | exp | public |
| native function | exp | public |
| native function | log | public |
| native function | log | public |
| native function | add | public |
| native function | add | public |
| native function | add | public |
| native function | add | public |
| native function | sub | public |
| native function | sub | public |
| native function | sub | public |
| native function | sub | public |
| native function | mul | public |
| native function | mul | public |
| native function | mul | public |
| native function | mul | public |
| native function | div | public |
| native function | div | public |
| native function | div | public |
| native function | div | public |
| native function | addScalar | public |
| native function | addScalar | public |
| native function | addScalar | public |
| native function | addScalar | public |
| native function | subScalar | public |
| native function | subScalar | public |
| native function | subScalar | public |
| native function | subScalar | public |
| native function | mulScalar | public |
| native function | mulScalar | public |
| native function | mulScalar | public |
| native function | mulScalar | public |
| native function | divScalar | public |
| native function | divScalar | public |
| native function | divScalar | public |
| native function | divScalar | public |
| native function | rsubScalar | public |
| native function | rsubScalar | public |
| native function | rsubScalar | public |
| native function | rsubScalar | public |
| native function | rdivScalar | public |
| native function | rdivScalar | public |
| native function | rdivScalar | public |
| native function | rdivScalar | public |
| native function | neg | public |
| native function | neg | public |
| native function | neg | public |
| native function | neg | public |
| native function | maxAxis | public |
| native function | maxAxis | public |
| native function | minAxis | public |
| native function | minAxis | public |
| native function | reluGrad | public |
| native function | reluGrad | public |
| native function | tanh | public |
| native function | tanh | public |
| native function | gather | public |
| native function | gather | public |
| native function | gather | public |
| native function | gather | public |
| native function | argmax | public |
| native function | argmax | public |
| native function | argmax | public |
| native function | argmin | public |
| native function | argmin | public |
| native function | argmin | public |
| native function | eq | public |
| native function | eq | public |
| native function | eq | public |
| native function | eq | public |
| native function | oneHot | public |
| function | uniform | public |
| function | uniform | public |
| native function | uniformF | public |
| native function | uniformD | public |
| function | arange | public |
| function | add | public |
| function | add | public |
| function | add | public |
| function | add | public |
| function | sub | public |
| function | sub | public |
| function | sub | public |
| function | sub | public |
| function | mul | public |
| function | mul | public |
| function | mul | public |
| function | mul | public |
| function | div | public |
| function | div | public |
| function | div | public |
| function | div | public |
| native function | max | public |
| native function | max | public |
| native function | max | public |
| native function | max | public |
| native function | min | public |
| native function | min | public |
| native function | min | public |
| native function | min | public |
| native function | sum | public |
| native function | sum | public |
| native function | sum | public |
| native function | sum | public |
| native function | sum | public |
| native function | sum | public |
| native function | sum | public |
| native function | sum | public |
| native function | max | public |
| native function | max | public |
| native function | max | public |
| native function | max | public |
| native function | min | public |
| native function | min | public |
| native function | min | public |
| native function | min | public |
| native function | mean | public |
| native function | mean | public |
| native function | mean | public |
| native function | mean | public |
| native function | mean | public |
| native function | mean | public |
| native function | mean | public |
| native function | mean | public |
| native function | dot | public |
| native function | dot | public |
| native function | dot | public |
| native function | dot | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | transpose | public |
| native function | flatten | public |
| native function | flatten | public |
| native function | flatten | public |
| native function | flatten | public |
| native function | slice | public |
| native function | slice | public |
| native function | slice | public |
| native function | slice | public |
| native function | broadcastTo | public |
| native function | broadcastTo | public |
| native function | broadcastTo | public |
| native function | broadcastTo | public |
| native function | reshape | public |
| native function | reshape | public |
| native function | reshape | public |
| native function | reshape | public |
| native function | concat | public |
| native function | concat | public |
| native function | concat | public |
| native function | concat | public |
| native function | matmul | public |
| native function | matmul | public |
| native function | matmul | public |
| native function | matmul | public |
| function | relu | public |
| function | relu | public |
| function | crossEntropyLoss | public |
| function | crossEntropyLoss | public |
| function | max | public |
| function | max | public |
| function | min | public |
| function | min | public |
| function | sigmoid | public |
| function | sigmoid | public |
| function | softmax | public |
| function | softmax | public |
| function | softmax | public |
| function | softmax | public |

Verdict: all 200 top-level declarations public; nothing to hide.

## thread.n

| Kind | Name | Effective access |
|------|------|------------------|
| native function | spawn | public |
| native function | join | public |
| native function | isFailed | public |
| native function | failure | public |
| native function | yield | public |
| native function | id | public |
| native function | sleep | public |
| class | Thread | public |
| class | Future | public |
| class | Promise | public |

Verdict: all 10 top-level declarations public; nothing to hide.

## url.n

| Kind | Name | Effective access |
|------|------|------------------|
| const class | UrlException | public |
| const class | Url | public |
| function | percentEncode | public |
| function | percentDecode | public |
| function | formEncode | public |
| function | formDecode | public |

Verdict: all 6 top-level declarations public; nothing to hide.

Total: 390 top-level declarations across 14 modules; verdict: the stdlib is a fully public curated face, matching D5.6's expectation (no hidden members, no cross-module denial risk introduced by the access gate).
