#!/usr/bin/env python3
"""Build three corpus books from short public-domain excerpts in Chinese, Japanese and Korean:
real text for the CJK line-breaking rules (CjkRulesTest) and their goldens.

  test_cjk_ja_soseki.epub   Natsume Soseki, Wagahai wa neko de aru, the opening of chapter 1, with
                            its ruby. Aozora Bunko, cards/000148/files/789_14547.html.
  test_cjk_zh_honglou.epub  Cao Xueqin, Hong Lou Meng (Dream of the Red Chamber), the opening of
                            chapter 1. Project Gutenberg eBook #24264. Its source line breaks are kept
                            inside each paragraph, as books converted from hard-wrapped text keep
                            them: they test the CSS segment-break rule.
  test_cjk_ko_hyeon.epub    Hyeon Jin-geon, Unsu joeun nal (A Lucky Day, 1924), the opening.
                            Korean Wikisource.

All three authors died before 1944; the texts are in the public domain. Only the text is taken: no
Project Gutenberg or Aozora markup, headers or trademarks. A leading ideographic-space indent is
replaced by CSS text-indent, so the first line of each paragraph starts indented.

Regenerate with:  python test/epubs/make_test_cjk_excerpts.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h1 { margin: 0 0 1em 0; text-indent: 0; font-size: 1.2em }
"""

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{title}</h1>
{body}
</body>
</html>
"""

# Wagahai wa neko de aru, chapter 1 (Aozora Bunko XHTML, ruby kept).
JA = [
    "<ruby><rb>吾輩</rb><rp>（</rp><rt>わがはい</rt><rp>）</rp></ruby>は猫である。名前はまだ無い。",
    "どこで生れたかとんと<ruby><rb>見当</rb><rp>（</rp><rt>けんとう</rt><rp>）</rp></ruby>がつかぬ。何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。吾輩はここで始めて人間というものを見た。しかもあとで聞くとそれは書生という人間中で一番<ruby><rb>獰悪</rb><rp>（</rp><rt>どうあく</rt><rp>）</rp></ruby>な種族であったそうだ。この書生というのは時々我々を<ruby><rb>捕</rb><rp>（</rp><rt>つかま</rt><rp>）</rp></ruby>えて<ruby><rb>煮</rb><rp>（</rp><rt>に</rt><rp>）</rp></ruby>て食うという話である。しかしその当時は何という考もなかったから別段恐しいとも思わなかった。ただ彼の<ruby><rb>掌</rb><rp>（</rp><rt>てのひら</rt><rp>）</rp></ruby>に載せられてスーと持ち上げられた時何だかフワフワした感じがあったばかりである。掌の上で少し落ちついて書生の顔を見たのがいわゆる人間というものの<ruby><rb>見始</rb><rp>（</rp><rt>みはじめ</rt><rp>）</rp></ruby>であろう。この時妙なものだと思った感じが今でも残っている。第一毛をもって装飾されべきはずの顔がつるつるしてまるで<ruby><rb>薬缶</rb><rp>（</rp><rt>やかん</rt><rp>）</rp></ruby>だ。その<ruby><rb>後</rb><rp>（</rp><rt>ご</rt><rp>）</rp></ruby>猫にもだいぶ<ruby><rb>逢</rb><rp>（</rp><rt>あ</rt><rp>）</rp></ruby>ったがこんな<ruby><rb>片輪</rb><rp>（</rp><rt>かたわ</rt><rp>）</rp></ruby>には一度も<ruby><rb>出会</rb><rp>（</rp><rt>でく</rt><rp>）</rp></ruby>わした事がない。のみならず顔の真中があまりに突起している。そうしてその穴の中から時々ぷうぷうと<ruby><rb>煙</rb><rp>（</rp><rt>けむり</rt><rp>）</rp></ruby>を吹く。どうも<ruby><rb>咽</rb><rp>（</rp><rt>む</rt><rp>）</rp></ruby>せぽくて実に弱った。これが人間の飲む<ruby><rb>煙草</rb><rp>（</rp><rt>たばこ</rt><rp>）</rp></ruby>というものである事はようやくこの頃知った。",
    "この書生の掌の<ruby><rb>裏</rb><rp>（</rp><rt>うち</rt><rp>）</rp></ruby>でしばらくはよい心持に坐っておったが、しばらくすると非常な速力で運転し始めた。書生が動くのか自分だけが動くのか分らないが<ruby><rb>無暗</rb><rp>（</rp><rt>むやみ</rt><rp>）</rp></ruby>に眼が廻る。胸が悪くなる。<ruby><rb>到底</rb><rp>（</rp><rt>とうてい</rt><rp>）</rp></ruby>助からないと思っていると、どさりと音がして眼から火が出た。それまでは記憶しているがあとは何の事やらいくら考え出そうとしても分らない。",
    "ふと気が付いて見ると書生はいない。たくさんおった兄弟が一<ruby><rb>疋</rb><rp>（</rp><rt>ぴき</rt><rp>）</rp></ruby>も見えぬ。<ruby><rb>肝心</rb><rp>（</rp><rt>かんじん</rt><rp>）</rp></ruby>の母親さえ姿を隠してしまった。その上<ruby><rb>今</rb><rp>（</rp><rt>いま</rt><rp>）</rp></ruby>までの所とは違って<ruby><rb>無暗</rb><rp>（</rp><rt>むやみ</rt><rp>）</rp></ruby>に明るい。眼を明いていられぬくらいだ。はてな何でも<ruby><rb>容子</rb><rp>（</rp><rt>ようす</rt><rp>）</rp></ruby>がおかしいと、のそのそ<ruby><rb>這</rb><rp>（</rp><rt>は</rt><rp>）</rp></ruby>い出して見ると非常に痛い。吾輩は<ruby><rb>藁</rb><rp>（</rp><rt>わら</rt><rp>）</rp></ruby>の上から急に笹原の中へ棄てられたのである。",
    "ようやくの思いで笹原を這い出すと向うに大きな池がある。吾輩は池の前に坐ってどうしたらよかろうと考えて見た。別にこれという<ruby><rb>分別</rb><rp>（</rp><rt>ふんべつ</rt><rp>）</rp></ruby>も出ない。しばらくして泣いたら書生がまた迎に来てくれるかと考え付いた。ニャー、ニャーと試みにやって見たが誰も来ない。そのうち池の上をさらさらと風が渡って日が暮れかかる。腹が非常に減って来た。泣きたくても声が出ない。仕方がない、何でもよいから<ruby><rb>食物</rb><rp>（</rp><rt>くいもの</rt><rp>）</rp></ruby>のある所まであるこうと決心をしてそろりそろりと池を<ruby><rb>左</rb><rp>（</rp><rt>ひだ</rt><rp>）</rp></ruby>りに廻り始めた。どうも非常に苦しい。そこを我慢して無理やりに<ruby><rb>這</rb><rp>（</rp><rt>は</rt><rp>）</rp></ruby>って行くとようやくの事で何となく人間臭い所へ出た。ここへ<ruby><rb>這入</rb><rp>（</rp><rt>はい</rt><rp>）</rp></ruby>ったら、どうにかなると思って竹垣の<ruby><rb>崩</rb><rp>（</rp><rt>くず</rt><rp>）</rp></ruby>れた穴から、とある邸内にもぐり込んだ。縁は不思議なもので、もしこの竹垣が破れていなかったなら、吾輩はついに<ruby><rb>路傍</rb><rp>（</rp><rt>ろぼう</rt><rp>）</rp></ruby>に<ruby><rb>餓死</rb><rp>（</rp><rt>がし</rt><rp>）</rp></ruby>したかも知れんのである。一樹の蔭とはよく<ruby><rb>云</rb><rp>（</rp><rt>い</rt><rp>）</rp></ruby>ったものだ。この垣根の穴は<ruby><rb>今日</rb><rp>（</rp><rt>こんにち</rt><rp>）</rp></ruby>に至るまで吾輩が<ruby><rb>隣家</rb><rp>（</rp><rt>となり</rt><rp>）</rp></ruby>の三毛を訪問する時の通路になっている。さて<ruby><rb>邸</rb><rp>（</rp><rt>やしき</rt><rp>）</rp></ruby>へは忍び込んだもののこれから先どうして<ruby><rb>善</rb><rp>（</rp><rt>い</rt><rp>）</rp></ruby>いか分らない。そのうちに暗くなる、腹は減る、寒さは寒し、雨が降って来るという始末でもう一刻の<ruby><rb>猶予</rb><rp>（</rp><rt>ゆうよ</rt><rp>）</rp></ruby>が出来なくなった。仕方がないからとにかく明るくて暖かそうな方へ方へとあるいて行く。今から考えるとその時はすでに家の内に這入っておったのだ。ここで吾輩は<ruby><rb>彼</rb><rp>（</rp><rt>か</rt><rp>）</rp></ruby>の書生以外の人間を再び見るべき機会に<ruby><rb>遭遇</rb><rp>（</rp><rt>そうぐう</rt><rp>）</rp></ruby>したのである。第一に逢ったのがおさんである。これは前の書生より一層乱暴な方で吾輩を見るや否やいきなり<ruby><rb>頸筋</rb><rp>（</rp><rt>くびすじ</rt><rp>）</rp></ruby>をつかんで表へ<ruby><rb>抛</rb><rp>（</rp><rt>ほう</rt><rp>）</rp></ruby>り出した。いやこれは駄目だと思ったから眼をねぶって運を天に任せていた。しかしひもじいのと寒いのにはどうしても我慢が出来ん。吾輩は再びおさんの<ruby><rb>隙</rb><rp>（</rp><rt>すき</rt><rp>）</rp></ruby>を見て台所へ<ruby><rb>這</rb><rp>（</rp><rt>は</rt><rp>）</rp></ruby>い<ruby><rb>上</rb><rp>（</rp><rt>あが</rt><rp>）</rp></ruby>った。すると間もなくまた投げ出された。吾輩は投げ出されては這い上り、這い上っては投げ出され、何でも同じ事を四五遍繰り返したのを記憶している。その時におさんと云う者はつくづくいやになった。この間おさんの<ruby><rb>三馬</rb><rp>（</rp><rt>さんま</rt><rp>）</rp></ruby>を<ruby><rb>偸</rb><rp>（</rp><rt>ぬす</rt><rp>）</rp></ruby>んでこの返報をしてやってから、やっと胸の<ruby><rb>痞</rb><rp>（</rp><rt>つかえ</rt><rp>）</rp></ruby>が下りた。吾輩が最後につまみ出されようとしたときに、この<ruby><rb>家</rb><rp>（</rp><rt>うち</rt><rp>）</rp></ruby>の主人が騒々しい何だといいながら出て来た。下女は吾輩をぶら下げて主人の方へ向けてこの<ruby><rb>宿</rb><rp>（</rp><rt>やど</rt><rp>）</rp></ruby>なしの小猫がいくら出しても出しても<ruby><rb>御台所</rb><rp>（</rp><rt>おだいどころ</rt><rp>）</rp></ruby>へ<ruby><rb>上</rb><rp>（</rp><rt>あが</rt><rp>）</rp></ruby>って来て困りますという。主人は鼻の下の黒い毛を<ruby><rb>撚</rb><rp>（</rp><rt>ひね</rt><rp>）</rp></ruby>りながら吾輩の顔をしばらく<ruby><rb>眺</rb><rp>（</rp><rt>なが</rt><rp>）</rp></ruby>めておったが、やがてそんなら内へ置いてやれといったまま奥へ<ruby><rb>這入</rb><rp>（</rp><rt>はい</rt><rp>）</rp></ruby>ってしまった。主人はあまり口を聞かぬ人と見えた。下女は<ruby><rb>口惜</rb><rp>（</rp><rt>くや</rt><rp>）</rp></ruby>しそうに吾輩を台所へ<ruby><rb>抛</rb><rp>（</rp><rt>ほう</rt><rp>）</rp></ruby>り出した。かくして吾輩はついにこの<ruby><rb>家</rb><rp>（</rp><rt>うち</rt><rp>）</rp></ruby>を自分の<ruby><rb>住家</rb><rp>（</rp><rt>すみか</rt><rp>）</rp></ruby>と<ruby><rb>極</rb><rp>（</rp><rt>き</rt><rp>）</rp></ruby>める事にしたのである。",
]

# Hong Lou Meng, chapter 1 (Project Gutenberg #24264), with its source line breaks.
ZH = [
    "此開卷第一回也．作者自云：因曾歷過一番夢幻之后，故將真事隱去，\n而借\"通靈\"之說，撰此《石頭記》一書也．故曰\"甄士隱\"云云．但書中所記\n何事何人？自又云：“今風塵碌碌，一事無成，忽念及當日所有之女子，一\n一細考較去，覺其行止見識，皆出于我之上．何我堂堂須眉，誠不若彼裙釵\n哉？實愧則有余，悔又無益之大無可如何之日也！當此，則自欲將已往所賴\n天恩祖德，錦衣紈褲之時，飫甘饜肥之日，背父兄教育之恩，負師友規談之\n德，以至今日一技無成，半生潦倒之罪，編述一集，以告天下人：我之罪固\n不免，然閨閣中本自歷歷有人，万不可因我之不肖，自護己短，一并使其泯\n滅也．雖今日之茅椽蓬牖，瓦灶繩床，其晨夕風露，階柳庭花，亦未有妨我\n之襟怀筆墨者．雖我未學，下筆無文，又何妨用假語村言，敷演出一段故事\n來，亦可使閨閣昭傳，复可悅世之目，破人愁悶，不亦宜乎？\"故曰\"賈雨村\n\"云云．",
    "此回中凡用“夢”用“幻”等字，是提醒閱者眼目，亦是此書立意本旨\n．",
    "列位看官：你道此書從何而來？說起根由雖近荒唐，細按則深有趣味．\n待在下將此來歷注明，方使閱者了然不惑．",
    "原來女媧氏煉石補天之時，于大荒山無稽崖練成高經十二丈，方經二十\n四丈頑石三万六千五百零一塊．媧皇氏只用了三万六千五百塊，只單單剩了\n一塊未用，便棄在此山青埂峰下．誰知此石自經段煉之后，靈性已通，因見\n眾石俱得補天，獨自己無材不堪入選，遂自怨自歎，日夜悲號慚愧．",
    "一日，正當嗟悼之際，俄見一僧一道遠遠而來，生得骨骼不凡，丰神迥\n异，說說笑笑來至峰下，坐于石邊高談快論．先是說些云山霧海神仙玄幻之\n事，后便說到紅塵中榮華富貴．此石听了，不覺打動凡心，也想要到人間去\n享一享這榮華富貴，但自恨粗蠢，不得已，便口吐人言，向那僧道說道：“\n大師，弟子蠢物，不能見禮了．适聞二位談那人世間榮耀繁華，心切慕之．\n弟子質雖粗蠢，性卻稍通，況見二師仙形道体，定非凡品，必有補天濟世之\n材，利物濟人之德．如蒙發一點慈心，攜帶弟子得入紅塵，在那富貴場中，\n溫柔鄉里受享几年，自當永佩洪恩，万劫不忘也。”二仙師听畢，齊憨笑道\n：“善哉，善哉！那紅塵中有卻有些樂事，但不能永遠依恃，況又有`美中\n不足，好事多魔'八個字緊相連屬，瞬息間則又樂极悲生，人非物換，究竟\n是到頭一夢，万境歸空，倒不如不去的好。”這石凡心已熾，那里听得進這\n話去，乃复苦求再四．二仙知不可強制，乃歎道：“此亦靜极慫級*，無中\n生有之數也．既如此，我們便攜你去受享受享，只是到不得意時，切莫后悔\n。”石道：“自然，自然。”那僧又道：“若說你性靈，卻又如此質蠢，并\n更無奇貴之處．如此也只好踮腳而已．也罷，我如今大施佛法助你助，待劫\n終之日，复還本質，以了此案．你道好否？\"石頭听了，感謝不盡．那僧便\n念咒書符，大展幻術，將一塊大石登時變成一塊鮮明瑩洁的美玉，且又縮成\n扇墜大小的可佩可拿．那僧托于掌上，笑道：“形体倒也是個寶物了！還只\n沒有，實在的好處，須得再鐫上數字，使人一見便知是奇物方妙．然后攜你\n到那昌明隆盛之邦，詩禮簪纓之族，花柳繁華地，溫柔富貴鄉去安身樂業。\n”石頭听了，喜不能禁，乃問：“不知賜了弟子那几件奇處，又不知攜了弟\n子到何地方？望乞明示，使弟子不惑。”那僧笑道：“你且莫問，日后自然\n明白的。”說著，便袖了這石，同那道人飄然而去，竟不知投奔何方何舍．",
    "后來，又不知過了几世几劫，因有個空空道人訪道求仙，忽從這大荒山\n無稽崖青埂峰下經過，忽見一大塊石上字跡分明，編述歷歷．空空道人乃從\n頭一看，原來就是無材補天，幻形入世，蒙茫茫大士，渺渺真人攜入紅塵，\n歷盡离合悲歡炎涼世態的一段故事．后面又有一首偈云：",
]

# Unsu joeun nal (Korean Wikisource).
KO = [
    "새침하게 흐린 품이 눈이 올 듯하더니 눈은 아니 오고 얼다가 만 비가 추적추적 내리었다.",
    "이날이야말로 동소문 안에서 인력거꾼 노릇을 하는 김 첨지에게는 오래간만에도 닥친 운수 좋은 날이었다. 문안에(거기도 문밖은 아니지만) 들어간답시는 앞집 마나님을 전찻길까지 모셔다 드린 것을 비롯으로 행여나 손님이 있을까 하고 정류장에서 어정어정하며 내리는 사람 하나하나에게 거의 비는 듯한 눈결을 보내고 있다가 마침내 교원인 듯한 양복장이를 동광학교(東光學校)까지 태워다 주기로 되었다.",
    "첫번에 삼십 전, 둘째 번에 오십 전 - 아침 댓바람에 그리 흔치 않은 일이었다. 그야말로 재수가 옴붙어서 근 열흘 동안 돈 구경도 못한 김 첨지는 십 전짜리 백통화 서 푼, 또는 다섯 푼이 찰깍하고 손바닥에 떨어질 제 거의 눈물을 흘릴 만큼 기뻤었다. 더구나 이날 이때에 이 팔십 전이라는 돈이 그에게 얼마나 유용한지 몰랐다. 컬컬한 목에 모주 한 잔도 적실 수 있거니와 그보다도 앓는 아내에게 설렁탕 한 그릇도 사다줄 수 있음이다.",
    "그의 아내가 기침으로 쿨럭거리기는 벌써 달포가 넘었다. 조밥도 굶기를 먹다시피 하는 형편이니 물론 약 한 첩 써 본 일이 없다. 구태여 쓰려면 못 쓸 바도 아니로되 그는 병이란 놈에게 약을 주어 보내면 재미를 붙여서 자꾸 온다는 자기의 신조(信條)에 어디까지 충실하였다. 따라서 의사에게 보인 적이 없으니 무슨 병인지는 알 수 없으되 반듯이 누워 가지고, 일어나기는새로 모로도 못 눕는 걸 보면 중증은 중증인 듯. 병이 이대도록 심해지기는 열흘 전에 조밥을 먹고 체한 때문이다.",
    "그때도 김 첨지가 오래간만에 돈을 얻어서 좁쌀 한 되와 십 전짜리 나무 한 단을 사다 주었더니 김 첨지의 말에 의지하면 그 오라질 년이 천방지축(天方地軸)으로 남비에 대고 끓였다. 마음은 급하고 불길은 닿지 않아 채 익지도 않은 것을 그 오라질 년이 숟가락은 고만두고 손으로 움켜서 두 뺨에 주먹덩이 같은 혹이 불거지도록 누가 빼앗을 듯이 처박질 하더니만 그날 저녁부터 가슴이 땅긴다, 배가 켕긴다고 눈을 홉뜨고 지랄병을 하였다. 그때 김 첨지는 열화와 같이 성을 내며,",
    "“에이, 오라질 년, 조롱복은 할 수가 없어, 못 먹어 병, 먹어서 병, 어쩌란 말이야! 왜 눈을 바루 뜨지 못해!”하고 김 첨지는 앓는 이의 뺨을 한 번 후려갈겼다. 홉뜬 눈은 조금 바루어졌건만 이슬이 맺히었다. 김 첨지의 눈시울도 뜨끈뜨끈하였다.",
    "이 환자가 그러고도 먹는 데는 물리지 않았다. 사흘 전부터 설렁탕 국물이 마시고 싶다고 남편을 졸랐다.",
]

BOOKS = [
    ("test_cjk_ja_soseki.epub", "ja", "吾輩は猫である", "夏目漱石", "一", JA),
    ("test_cjk_zh_honglou.epub", "zh-Hant", "紅樓夢", "曹雪芹", "第一回", ZH),
    ("test_cjk_ko_hyeon.epub", "ko", "운수 좋은 날", "현진건", "운수 좋은 날", KO),
]


def write_book(name, lang, title, author, chapter_title, paragraphs):
    body = "\n".join(f"  <p>{p}</p>" for p in paragraphs)
    chapter = CHAPTER.format(lang=lang, title=chapter_title, body=body)
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        f"    <dc:creator>{author}</dc:creator>\n"
        f"    <dc:language>{lang}</dc:language>\n"
        f'    <dc:identifier id="bookid">{name}</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        '    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="chapter1"/>\n  </spine>\n'
        "</package>\n"
    )
    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )
    out = os.path.join(HERE, name)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", chapter)
    print("wrote", out)


def main():
    for book in BOOKS:
        write_book(*book)


if __name__ == "__main__":
    main()
