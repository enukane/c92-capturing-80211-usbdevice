= フレームが手元に届くまで

この章では、ドライバがデバイスから802.11フレームを受け取って
ユーザランドのキャプチャソフトウェア(Wiresharkなど)
に至るまでの流れを追いかけていきます。
キャプチャにあたって関係するコンポーネントや情報の流れを見ていきます。

== ユーザから見たキャプチャのしかた

下回りで何が行われているのかを見ていく前に、
まずは基本となるキャプチャのお作法・手順をおさらいしておきましょう。
802.11インタフェースにてキャプチャを行うには以下の様に
iwコマンドとipコマンドを用いてインタフェースの設定を変更し、
Wiresharkやtsharkあるいはairodump-ngといったソフトウェアを
用いてフレームの取得を行います@<fn>{mon0}。

//footnote[mon0][ここではwlan0自体をモニタモードに設定していますが、mon0といったサブインタフェースを切り出す方法もあります]

//cmd{
# 1. モード変更のためインタフェースを一度 DOWN にする
% ip link set wlan0 down

# 2. wlan0 を Monitor モードに設定、以下のフレームを受信するよう設定
#  - FCSエラーのフレーム
#  - 他のAP行きのフレーム
#  - Control フレーム
% iw wlan0 set monitor fcsfail otherbss control

# 3. インタフェースを一度 UP にする (状態反映のため)
% ip link set wlan0 up

# 4. パケットキャプチャを行う初期チャネルを設定する(初期値は 1)
# インタフェースが UP でないとチャネル変更できないことに注意
% iw wlan0 set channel 1

# 5. 任意のキャプチャソフトウェアでフレームを収集
% tshark -i wlan0 -w output.pcapng

# 6. UP している限りは, tshark 実行中でも任意にチャネルを変更可能
% iw wlan0 set channel 36
//}

== 全体像

802.11キャプチャにあたっては「設定」と「受信」の2種類のパスで
オペレーションを行います。
#@# @<img>{general-picture}に図示されるそれぞれのパスに現れる
#@# コンポーネントを通ってこれが実行されます。
#@# 以降はそれぞれのパスの詳細を見ていきます。

//image[general-picture][設定パス、受信パスの全体像][scale=0.6]

== 設定パス

設定パスは主にインタフェースのモニタモードへの変更、
インタフェースの有効化による受信の開始をするために通過するパスです。
#@# それぞれiwコマンド、ipコマンドに従って @<img>{config-path} に図示される
#@# コールグラフをたどります。

//image[config-path][設定パスのコールグラフ][scale=0.6]

iw set monitor コマンドを実行すると、libnl 経由でカーネル側 nl80211 モジュールの
NL80211_CMD_SET_INTERFACEコマンドが呼ばれます。
このハンドラ関数としては nl80211_set_interface が待ち構えており、
set monitorコマンドの引数として与えた "fcsfail otherbss control"
といった文字列をフラグに変換します。
nl80211 では引数文字列として以下を受け付けます。
基本的には前3つを利用することが多いです。

  * otherbss: ハードウェアでのBSSIDフィルタリングを解除(いわゆるプロミスキャスモード)
  * control: コントロールフレーム(wlan.fc.type == 0x10)も採取
  * fcsfail: FCSエラーと判定されたフレームも採取
  * plcpfail: PLCPエラーと判断されたフレームも採取
  * cook: フレームを通常通り処理した上で採取 (他のフラグとは排他)
  * active: モニタモードだが通常どおりACKを返す(フレームの送信が行われる)

#@# 中でもotherbssは必須です。
#@# イーサネットデバイス同様に、802.11デバイスでも自身のアドレスまたはマルチキャスト
#@# アドレス宛のフレームはハードウェア側でフィルタしソフトウェアには上がってこない
#@# ように設定されます。これを解除するためのパラメータがotherbssです。

iw set monitor コマンドでモニタモードに設定した時点では、インタフェースは DOWN 状態です。
このため、設定パスでは管理情報の変更のみが行われ実際にデバイスの状態を変えるところまでは完了しません。
このパスの最終地点である ieee80211_setup_sdata ではモニタモードとして
の初期化とフラグの保存のみが行われます。


実際の切り替えやハードウェアへの反映は、インタフェースが UP 状態になった時に改めて行われます。
これが ip link set up から続く設定パスです。
先に保存しておいたフラグ等は ieee80211_do_open で呼ばれる
ieee80211_adjust_monitor_flags にて処理され
ieee80211_local 構造体の対応するメンバ(fif_other_bss、fif_fcsfailなど)の状態が変わります。
これに基づいて、その直後に呼ばれる ieee80211_configure_filter がデバイスまで状態を反映させます。

例として Ralink社製デバイス向けのドライバである rt2800usb では
rt2x00mac_configure_filter がこれを担います。
この関数ではハードウェアのケーパビリティに基づいて
フラグの調整を行った後にrt2800_config_filterでハードウェアの状態を変更します。
フラグに対応して以下のフィルタレジスタの値として適切なものが書き込まれます。

#@# : fcsfail
#@#   RX_FILTER_CFG_DROP_CRC_ERROR
#@# : plcpfail
#@#   RX_FILTER_CFG_DROP_PHY_ERROR
#@# : control
#@#   RX_FILTER_CFG_DROP_CF_END_ACK, RX_FILTER_CFG_DROP_CF_END, RX_FILTER_CFG_DROP_ACK, RX_FILTER_CFG_DROP_CTS,
#@#    RX_FILTER_CFG_DROP_RTS, RX_FILTER_CFG_DROP_BAR, RX_FILTER_CFG_DROP_CNTL

//list[rt2800_config_filter][rt2800usb で設定されるフィルタ]{
fcsfail
  RX_FILTER_CFG_DROP_CRC_ERROR
plcpfail
  RX_FILTER_CFG_DROP_PHY_ERROR
control
  RX_FILTER_CFG_DROP_CF_END_ACK, RX_FILTER_CFG_DROP_CF_END, 
  RX_FILTER_CFG_DROP_ACK, RX_FILTER_CFG_DROP_CTS,
  RX_FILTER_CFG_DROP_RTS, RX_FILTER_CFG_DROP_BAR, RX_FILTER_CFG_DROP_CNTL
//}


名前から察せられるように対応するサブタイプや状態のフレームのドロップの
有無を制御できるようになっています。
これに加えてモニタモードであれば RX_FILTER_CFG_DROP_NOT_TO_ME を変えて
RX_FILTER_CFG レジスタに書き込むことで、ハードウェアでよしなに処理し
ドロップしてしまうフレームをソフトウェアで受信することが出来るようになります。
ここまでの流れにて、拾いたいフレームが拾える状態になり
インタフェースが有効化され802.11フレームキャプチャのお膳立てが完了したことになります。

== 受信パス

前節の流れで、ソフトウェア的にモニタモードとして設定され
これに紐付いてハードウェアが通常では渡してこないフレームを
受け取れるような設定が為されました。
ここでは実際に rt2800usb を例に受信パスについて追っていきます。
コールグラフは @<img>{receive-path} に図示される流れになります。

//image[receive-path][受信パスのコールグラフ][scale=0.6]


一般的にモニタモードと通常のモードでもフレームの受け取り方に差異はなく
通常の受信パスにいつもより多くの種類のフレームが押し寄せるだけです。
このためモニタモードでも、デバイスから割り込みを受けて受信キューを漁り
受信完了となったフレームを順繰りに探し出して処理してから上位層に投げる、
といういつもの仕事が行われます。

rt2800usb では、rt2x00usb_interrupt_rxdone が受信完了割り込みをUSBサブシステムから受けます。
実際の受信処理はここから workqueue を介して呼び出される
rt2x00usb_work_rxdone のコンテキストで行われ、128個と設定されているRXキューからの
フレーム引き上げ作業が行われます。
キャプチャにおいて後ほど重要となるパケット外のメタ情報である
ieee80211_rx_status構造体への情報設定は rt2x00_rxdone にて行われます。
この関数ではハードウェアの受信デスクリプタを取得しそこに記載された情報を
ベースにこの構造体のメンバを埋めていきます。


ドライバでの処理が終わったフレームは順に802.11スタックへと引き渡されていきます。
通常の受信処理ではデフラグやMPDUの並べ替えなどを行ったのちにさらに上位の層
への引き渡しとなりますが、モニタモードではこれらが不要なため ieee80211_rx_monitor
にて802.11スタックでのお仕事は終わりになります。

ieee80211_rx_monitor でもっとも重要なタスクは Radiotap ヘッダの仕込みです。
これは ieee80211_add_rx_radiotap_header 関数にて行われ、
先にドライバで埋めた ieee80211_rx_status 構造体やハードウェア情報に従って決定されます。
具体的にどのような要素により Radiotap ヘッダの各部が決められるかの詳細は
後ほど解説します。


ここまでデバイスドライバから802.11スタックまで駆け上がってきたフレームを
さらにユーザランドまで引っ張りあげるために PF_PACKET が用いられます。
これはカーネル内を通過するパケットをユーザランドから取得する
手段として提供されている仕組みで、PF_PACKET としてソケットを開きインタフェースや
フィルタを設定することで、指定のインタフェースで送受信されるパケットのコピーを
ユーザランドにて取得することができます。
Linux 版の libpcap では下回りとしてこの PF_PACKET を用いており、このライブラリに
依存している tcpdump や Wireshark もその恩恵にあずかっています。

PF_PACKET を利用する場合、カーネル内にユーザランドからmmapにより参照可能な
領域が作られこれを経由してフレームの引き渡しが行われます(図中のRX_RING)。
カーネル側では、802.11スタックから入ってきたフレームをaf_packetのtpacket_rcvにて
リングバッファとして扱われるこの領域にコピーし、RX_RINGで示されるキューに管理情報を積みます。
ユーザランド側ではpollによりソケットを監視し、変化があった == 受信フレームがある
としてリングバッファに入っているフレームを参照します。
この仕組みによりユーザとカーネルのコンテキストを跨がったコピーなしに
高速にフレームを取得することができます。

//footnote[packet_mmap][https://www.kernel.org/doc/Documentation/networking/packet_mmap.txt]


== パケットに紐付く情報

ここまでは802.11フレームをキャプチャソフトウェアで引っ張りあげてくるところまでの
お話をしてきました。これでめでたくフレームが手に入るわけですが、
実際にキャプチャデータを見てみると
802.11フレーム本体(この場合、IEEE 802.11 QoS Data 以下)の手前に
先述の @<em>{Radiotap Header}や@<em>{802.11 radio information}なるものが存在しているのが
見えます。
この役割や具体的な各フィールドの意味についてはリックテレコム社の「パケットキャプチャ 無線LAN編」
@<fn>{ikeriri-80211-book}やRadiotap Defined Fields @<fn>{radiotap-defined-fields} をご参照頂くとして、
ここではこの中身の出所について見ていきます。

//footnote[ikeriri-80211-book][ISBN978-4-86594-029-9]
//footnote[radiotap-defined-fields][http://www.radiotap.org/fields/defined]

#@# //image[80211-frame-hdr][802.11フレームの各種ヘッダ]

=== Radiotapヘッダ

Radiotapヘッダの実体は、先にも述べたようにドライバ側が埋める ieee80211_rx_status 構造体や
ハードウェア情報(ドライバが持っているコンテキスト)に依存して構成され
802.11フレームの手前の領域に付加されるヘッダです。
情報のソースとなる ieee80211_rx_status 構造体のメンバは @<img>{rxstatus_member} に示される形式で定義されています。

//image[rxstatus_member][ieee80211_rx_status構造体]

この構造体のメンバの中で特に重要なのはflagメンバで、
このビットの有無に従って要素を継ぎ足しながら Radiotap ヘッダは構成されていきます。
各ビットの意味は @<img>{rxstatus_flags} の表に記載した通りです。
//image[rxstatus_flags][RX_FLAG_*: ieee80211_rx_statusのflag]

ここからは Radiotap ヘッダの主要なフィールドの由来について見ていきます。
まずは @<img>{radiotap_legacy} に共通に使われるフィールドの由来を載せています。
#@# radiotap.datarate の参照には注意が必要で 802.11n または 802.11ac の場合は
#@# このフィールドは空になり Wireshark では見えなくなります。
#@# これらのモードの場合は後述するMCS Indexやストリーム数から割り出す必要があります。
最後の「dbm_antsignalとantenna」の繰り返しは @<img>{radiotap_antsignal_multi} に
あるような形式で見えるようになります。
この例では 802.11ac で4つのアンテナ(0〜3)について radiotap.dbm_angsignal と
radiotap.antenna が併記されています。
いずれも同じフィールド名で重複しています。

//image[radiotap_antsignal_multi][複数のdbm_antsignalとantenna (802.11ac 4x4 MIMO の例)][scale=1.0]

//image[radiotap_legacy][Radiotapヘッダ: 基本のフィールド(共通)]

802.11nでサポートされる変調方式で受信したフレームについては、
@<img>{radiotap_ht} に記載される形式のフィールドが付きます。
802.11n での転送レートはここに含まれるMCS Index (radiotap.mcs.index) から取得する必要があります。
Mbps 形式での値はいずれのフィールドにも入っておらず、
Wiresharkでも計算してくれないため必要な場合は自分で計算する必要があります。

//image[radiotap_ht][Radiotapヘッダ: 802.11n の場合のフィールド]

802.11n 同様に 802.11ac の場合も @<img>{radiotap_vht} に記載される
フィールドが別に用意されています。
802.11ac からは MCS Index の形式がストリーム数に依らず 0-9
の範囲で固定化されたため MCS Index (radiotap.vht.mcs.*)
のほかにストリーム数 (radiotap.vht.nss.*) が分からないと
転送レートが定まらないためこの2つが必要となります。

//image[radiotap_vht][Radiotapヘッダ: 802.11nacの場合のフィールド]

=== Radio Information

Radio Information は Radiotap ヘッダや802.11フレームと異なり実体のあるデータではなく
擬似的なフィールドになります。
@<img>{radioinfo-source}に一覧するように基本的にはRadiotapヘッダの
内容を再解釈し分かり易くしたものになっています。

//image[radioinfo-source][802.11 Radio Information: 主要フィールドの出所]

以下はそれぞれ 802.11a, 802.11n, 802.11ac での当該フィールドのサンプルです。

//image[radioinfo-column][802.11 Radio Information: 802.11a, n, acの場合]
