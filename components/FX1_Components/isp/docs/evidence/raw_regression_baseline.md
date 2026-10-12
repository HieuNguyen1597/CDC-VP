# FX1 ISP real-RAW regression baseline

FX1 ISP real-RAW regression baseline: SHA-256 of every frame's NV12 (active bytes) and of the statistics readout after the last frame, model and Python reference bit-exact (DEC-05: project reference, not an owner golden).

Generated 2026-10-12T15:52:55 from git e51b32550156 (dirty tree) by `tools/run_regression.py --write-baseline`. 116 runs, 308 frames; all bit-exact with the reference.

| kind | RAW | preset | output | frame | NV12 SHA-256 |
|---|---|---|---|---|---|
| image | khoid_ground_1_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `272ff227ae010a6cd2c90b972265f4f4f7f987805a2a49d8820deda64040709e` |
| image | khoid_ground_1_raw_2688x1520_5376 | full | 1920x1080 | 0 | `cedc06b77cab5ee22e873856926cddb5d273f17bfcd468a7bab23d80b2a16efb` |
| image | khoid_ground_1_raw_2688x1520_5376 | full | 1920x1080 | 1 | `a9cc194fe67e8be1b9ad1d45a9f86a5ea7144036e785a93b89249585cd123cf3` |
| image | khoid_ground_1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `d2817e9686a4c429c75907634c01f8ab8681434ed9ef1afcd561ad3ea9d6ce41` |
| image | khoid_ground_1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `922f9b4d643c554a26cfffe6940a8b86459e97018499e64bc8fc78bf5584c12f` |
| image | khoid_ground_1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `e9d18e84b7559fa09e73e80e51e80a9ee8a46b976434e432672375de85aed55d` |
| image | khoid_ground_1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `e9d18e84b7559fa09e73e80e51e80a9ee8a46b976434e432672375de85aed55d` |
| image | khoid_ground_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `fe02aaeff883beb42638e16aa825c784bf23dbba46e01c67c78e310389560255` |
| image | khoid_ground_raw_2688x1520_5376 | full | 1920x1080 | 0 | `8bddad98388946f612f579111897dd326816eb7c20355f0f037ef7f318eebb85` |
| image | khoid_ground_raw_2688x1520_5376 | full | 1920x1080 | 1 | `20efd004610ff5b0b6d83009b999a3648dee4bd8c76add08a7929ce7f8e52a4f` |
| image | khoid_ground_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `39a0006368502af7cbdc02a69e3f552907bfaa1102243d4721ddbe7c395bfa1b` |
| image | khoid_ground_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `ff2cd731cf2e942e5790cb563cb83ce7b4d03172cff7ba273333684b8c8c1de6` |
| image | khoid_ground_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `a12932a842597f1917fcf8a163156eaf007bde462e871f5043372d0babd0e7cd` |
| image | khoid_ground_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `a12932a842597f1917fcf8a163156eaf007bde462e871f5043372d0babd0e7cd` |
| image | khoid_indoor_led_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `5baf97df8dea4ab30b8cc14d053f9c6bd057149a552b15304f1ed03ab26345b4` |
| image | khoid_indoor_led_raw_2688x1520_5376 | full | 1920x1080 | 0 | `3666004d3497ff50439eee782897af1a781fe35b16c101ad05690ec5c8eef2f8` |
| image | khoid_indoor_led_raw_2688x1520_5376 | full | 1920x1080 | 1 | `de3fa726dc695150d5606d3e8971de6d461fc87be1c619db38ecf95cdf20bf29` |
| image | khoid_indoor_led_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `97e2de53edfbf0babf55cbd6a728022212466c45ea0bb72ae58e995c25f28718` |
| image | khoid_indoor_led_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `c707e1fd8f1b0bdeaf9db516c6cf0e93fe65b32046b411cd38bfcc870f98ec08` |
| image | khoid_indoor_led_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `04997bc867403d0559c538e5399855983cb4561bdf5268ca219b465e43789226` |
| image | khoid_indoor_led_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `04997bc867403d0559c538e5399855983cb4561bdf5268ca219b465e43789226` |
| image | khoid_outdoor1_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `a785773b7fabbe0c8166929b2db5b984a4afef7d83b4524c32696a054ceba472` |
| image | khoid_outdoor1_raw_2688x1520_5376 | full | 1920x1080 | 0 | `871f0f88bb69dcd45b926eda8386cf5c461fefb8bd0f269d7056cee37d4b02d2` |
| image | khoid_outdoor1_raw_2688x1520_5376 | full | 1920x1080 | 1 | `b277e5cebb9125020d2c568c4df2fb7575f12d5a099d85613624dc8f1fcddac2` |
| image | khoid_outdoor1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `f8bb290e54f54ec8515d30a65333810437baf27be9e1d293a1b79aeed489a254` |
| image | khoid_outdoor1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `9bd90c571c1a0aedcaabc6e6e34ce728d944dc2a1a153fc4464f2f15a7302c82` |
| image | khoid_outdoor1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `ce95f8582d06a75e347bae534e7796233f8b6c5623981340ea03e89b28b925a5` |
| image | khoid_outdoor1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `d3198e88a278ea1d3d08457fa44cfd04e4556f5ad4d42fa5994d9720aba36735` |
| image | khoid_outdoor2_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `959832490cbe5796164649108bc410616a7bc87752c646f8a5319b6f6f820bae` |
| image | khoid_outdoor2_raw_2688x1520_5376 | full | 1920x1080 | 0 | `6f7d758791fbdc970bf02efb1b88c0fe8f16e47dba45d22b28ca8a2b4fda7deb` |
| image | khoid_outdoor2_raw_2688x1520_5376 | full | 1920x1080 | 1 | `c2faf7ce8ed8000f2a11abd3031b51ad0389c67b554956131ff3cb818e2cf7d4` |
| image | khoid_outdoor2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `4aad9a2328855d45291ef96ddae2b96710a18759026fbb50b31fa3b072412cf5` |
| image | khoid_outdoor2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `7156e5c5d216c161e5237633f6504db62bd8a5078fef2d4e2e30dc73c4c7b580` |
| image | khoid_outdoor2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `68ae6f8ca87d1f0067bd9ae81fdbefc86ccbf3e0358566b2e63b2c273ac5d3ac` |
| image | khoid_outdoor2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `6def6b8f050fefd4397abd164ff2e089cf7691692b7a72b4eddb4d0972d8f1a7` |
| image | khoid_outdoor3_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `e352621cfebab37a4f972dd13dcd269c8ba2e46c203fb493cef79f31f200fce8` |
| image | khoid_outdoor3_raw_2688x1520_5376 | full | 1920x1080 | 0 | `06be225358bb5bbbe3f3aefd1dc53eee4de41dfa649810363bbafed12c66ae02` |
| image | khoid_outdoor3_raw_2688x1520_5376 | full | 1920x1080 | 1 | `5ae70875cc4df576e71dae437df8f212578c1a896fe04d49d5d4489f734a0e77` |
| image | khoid_outdoor3_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `48545fcb3dbced1a1b1a34a1dc65634e4cd6c2b4cb98b53ef13226893a81b67e` |
| image | khoid_outdoor3_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `416dbdf4b28eaf3c4376dc9b65a9710327ffbec4054dfcd6bb218a361b20cc22` |
| image | khoid_outdoor3_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `9d5f4e4fee0423b2e5ca8002c041eef6c5295e40c9571b97782accfc98a5a694` |
| image | khoid_outdoor3_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `9d5f4e4fee0423b2e5ca8002c041eef6c5295e40c9571b97782accfc98a5a694` |
| image | khoid_outdoor4_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `ae2ac05821b62eb1b6efff90411ca9f91cdb8631acc067df8d723bf0793e9fd5` |
| image | khoid_outdoor4_raw_2688x1520_5376 | full | 1920x1080 | 0 | `99255a42a63c51eca56476c18d457e258fcc7faed7576a618888c992946dc343` |
| image | khoid_outdoor4_raw_2688x1520_5376 | full | 1920x1080 | 1 | `ba4ecce5509d5dc0c0587e2a6460f8302823efc326038d97ed8f698ed335bb25` |
| image | khoid_outdoor4_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `9f1cb3d8ba206ad55c1a0ccdf3b13a5117b5b9d894c9f0cbb6f2cf43cc6bb95d` |
| image | khoid_outdoor4_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `bead857ad165f279e023034fa8c8c6d7379b7b437208c32ddc84bb5b4654992e` |
| image | khoid_outdoor4_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `6799b30c650edcbae88317c179b05cfcb266194a2cc81f9b1f808eddb44bc499` |
| image | khoid_outdoor4_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `b4ce11db5c030fb0ceb448f4d4467b7f0f16bc36e5fccddb314d63cccfabef5f` |
| image | khoid_outdoor5_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `54f98da5d3eb5021fbc6ca165e84c8c28e24fcac2ce43e037c79950f937c6586` |
| image | khoid_outdoor5_raw_2688x1520_5376 | full | 1920x1080 | 0 | `84dd54d43722edc6918c683dcc314828897596c3433fea53d65d40274f060364` |
| image | khoid_outdoor5_raw_2688x1520_5376 | full | 1920x1080 | 1 | `a5123279aa4361cdf6bbd5418ea8383fcf06c9dc983d4de08f8e557a31eb8046` |
| image | khoid_outdoor5_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `77a5cd4facdc81de18ba2c37d57b0ee07bd9f0861105ca50bf5642a0fad661ae` |
| image | khoid_outdoor5_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `1fd3f00acbd1c90a4487cd7f2246f3a08a8dbbc30e1600b80dd8fcac69242217` |
| image | khoid_outdoor5_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `c6ee6219d5ffca02efc786538abd5427d7f0f598c6a922fa8197ecfe7a437585` |
| image | khoid_outdoor5_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `22bed7a0b9611e7aa7f7cd6701ba984b3317ae09a0046e5f5b0c8ac0d1ec4126` |
| image | khoid_outdoor6_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `dd8d0d5833f19deeaa4c01d9c942dbba51a207371d26ccf8e64fadde75854b46` |
| image | khoid_outdoor6_raw_2688x1520_5376 | full | 1920x1080 | 0 | `892fe1f1b5be76aa6e591ca5baed0174bb096e7e868c7c298483bd154e23d5e7` |
| image | khoid_outdoor6_raw_2688x1520_5376 | full | 1920x1080 | 1 | `7e12b8fee972cd134e7fea794baac67a5f44ac07d1f999ea459fc6547cc3ea21` |
| image | khoid_outdoor6_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `476c812023c0cc89b6eb9d96157bb7857da9306732b442fd21f6929951457948` |
| image | khoid_outdoor6_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `44b9a0d4d362ee8b25672c162f435f029e235784b1a5afe7b41dbf7d5a312a77` |
| image | khoid_outdoor6_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `b4445b3fe36cc15185523a42194806e4dd0ac372c0b154934a23632146172b35` |
| image | khoid_outdoor6_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `13b70f5f3cf537e63d10f59d755eba584f385ac206e94c7ea2e1ed4ec0918382` |
| image | khoid_outdoor7_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `5c15c889f091c6694698c724454da1c8ad7d455b934bbf32b8df4ef3024316eb` |
| image | khoid_outdoor7_raw_2688x1520_5376 | full | 1920x1080 | 0 | `7ef6f7a4e88fad9b589ae80f7f2a260308820bf352012c616186a531691b3616` |
| image | khoid_outdoor7_raw_2688x1520_5376 | full | 1920x1080 | 1 | `39180863ccbba5c96f11f5906872e13fa9d1e2c6e48f067d3af43ab326c9a610` |
| image | khoid_outdoor7_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `10bd1a37ce81979f1dcef31575a74260de9328b30634b8a45e5fce0f14556b05` |
| image | khoid_outdoor7_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `365814e4f9b50bfbb5896351fb1c731a85b76ee5818a63a2d76ca36434cd7817` |
| image | khoid_outdoor7_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `3ed1ee2c54107364c8caeedf31682127daa971862e9f93cd9cefb33c3a163cac` |
| image | khoid_outdoor7_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `3d963656cdf7e120050e60c033f779fb32c645fa177f951565837993bc66dfdd` |
| image | khoid_outdoor8_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `1c05d2fce725faa9fdcbdfc692804845473131ae039b6ea9fdeb6a80c27f56a2` |
| image | khoid_outdoor8_raw_2688x1520_5376 | full | 1920x1080 | 0 | `1ae50c58bc548b658efa0b8e22c6fabb6fc7545aafffabdccb6ea8604c34ef9d` |
| image | khoid_outdoor8_raw_2688x1520_5376 | full | 1920x1080 | 1 | `0f88b5d02e29530b61a8e9ad402f4361419a7d34e244fbcbdcbdc74c318db21c` |
| image | khoid_outdoor8_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `6bd29083c5a1b38021fc3a6520bb45a860f7484c9878ca035215dbc5e495aa0d` |
| image | khoid_outdoor8_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `87567f3b5734ea477ada2c03ea9836bfe26433188c18aef9623ff8b35b13a1e6` |
| image | khoid_outdoor8_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `f5ef42aad0ec94f7cf416c89e04dc69effbb94f7438bf4ae05f05358a9d7a3c5` |
| image | khoid_outdoor8_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `f5ef42aad0ec94f7cf416c89e04dc69effbb94f7438bf4ae05f05358a9d7a3c5` |
| image | khoid_outdoor_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `c4f8b214dda77b4c124d3164f162eaaeb128906006cd8fb41123fcca1019be78` |
| image | khoid_outdoor_raw_2688x1520_5376 | full | 1920x1080 | 0 | `5006d3e71219e73b2f2802a12c01ddee81083886525ed369162fb44314eef3aa` |
| image | khoid_outdoor_raw_2688x1520_5376 | full | 1920x1080 | 1 | `64716f209e4549a6aa366274cf9398ddc0ab2fea249ae8217bdc021933b7c803` |
| image | khoid_outdoor_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `d092635dbbbe402cf4af92e14f175645d8e64b13ff3fcbee3b0266c814a19636` |
| image | khoid_outdoor_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `c4e3aab9f3c13982538f7c059ea6002056979112b1c2883e056945d15b2bbada` |
| image | khoid_outdoor_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `34913385f0c2242af23521b86e8d94c6708a7d461ce60480405601498ed51bb9` |
| image | khoid_outdoor_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `34913385f0c2242af23521b86e8d94c6708a7d461ce60480405601498ed51bb9` |
| image | khoipd_basement1_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `f1cdc84f9b73b1f0ea500a43bb29cbf053250728d7a85eb9f021a2c2f8ae031f` |
| image | khoipd_basement1_raw_2688x1520_5376 | full | 1920x1080 | 0 | `0cdf1258539ea2047761551f12cbb4148bae28b1c1043f57df9928761199ac36` |
| image | khoipd_basement1_raw_2688x1520_5376 | full | 1920x1080 | 1 | `c5218b9b2cc556d957395c91173b5fa2f51a0bca6c92297ed3405d3de38b2dc2` |
| image | khoipd_basement1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `d95807b504e8d0e201f9f478fa5a6779118f831a5eb2e1fc61b685bd2d643369` |
| image | khoipd_basement1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `e1f26a27604a5706bbed6dbf3a68b5e760cfa03f7720dab2c19c61a04e867b62` |
| image | khoipd_basement1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `5dfd67cc1e672a011c64b3106c8b2df7f6e98e10dc7ca770785705abfd430620` |
| image | khoipd_basement1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `a7cc64cfcd48e0f2e61ce5f080db162d523abae7b3fe491e859062770b00af86` |
| image | khoipd_basement2_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `96bfef1e046c9b97b501a73de3d2db9eab8755441d9074bda72fe5c97d6c558c` |
| image | khoipd_basement2_raw_2688x1520_5376 | full | 1920x1080 | 0 | `fee19afb53109d08f228a110ba2574e52281c8b4df7daa625f2c672de9902891` |
| image | khoipd_basement2_raw_2688x1520_5376 | full | 1920x1080 | 1 | `e72da10a8a611299fee3a05d162bba9c4cbef08e3f86f5c2672b2b661a650a22` |
| image | khoipd_basement2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `f7392429fb210b871aba9a7d821aa5509a6c39803f1f4a38e0741e5e2673fff9` |
| image | khoipd_basement2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `7385d9be63ab01581fc4e67026316124c86da97f796555b143552fe00d9d80da` |
| image | khoipd_basement2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `d1c5238bbbbf2003e0ed7b2f623bc67fcc651ed800a70d1d6ebceefdd73aad48` |
| image | khoipd_basement2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `83bc51a67dcdf8626d486a8ec07a5cc18cef0d78beb7dc398295ecb26d082da3` |
| image | khoipd_basement_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `6fdf354a46b385be0abbf64b77627334d3ede3907e33609e89500d2be7621696` |
| image | khoipd_basement_raw_2688x1520_5376 | full | 1920x1080 | 0 | `1314006ae17af512980f92f03a3a02b8385d934e0a58dcf6d9085e12fa310fba` |
| image | khoipd_basement_raw_2688x1520_5376 | full | 1920x1080 | 1 | `a4d35efbbaf4683c3510dcbaf02841cbbce5038f64cf15f0d2b304d189f667e9` |
| image | khoipd_basement_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `3c1e16a805b8d328f7246168ed8b94324a2d9ad470791eb7a53c53543da51265` |
| image | khoipd_basement_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `0af5170df4c137168f9c953ddf4b1b26fb1f3ba6e3302dd2ef5db05d0bb026ab` |
| image | khoipd_basement_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `353d9d0a9ff53be1769b0d8c13e667209ebae7768fef82ba71af6a9185008542` |
| image | khoipd_basement_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `f6af5b621ddb54d3815ccf1926450a59e302cc5b85d5a3bdea7b587037714ff5` |
| image | khoipd_indoor1_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `b9e2d98ee6bc282bfd3df832d96a4d8b7d714c7d9a5c73c457be5925b86d0302` |
| image | khoipd_indoor1_raw_2688x1520_5376 | full | 1920x1080 | 0 | `daf8eeb4e7bb4cc689e1182c4ec5cacf30a6bc67482b7841919c936c55fbbaef` |
| image | khoipd_indoor1_raw_2688x1520_5376 | full | 1920x1080 | 1 | `f3852bbb9c224f962360ab2b381b80dcb88106e9f23335fa0d8535b91d42d773` |
| image | khoipd_indoor1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `277972ad99b7a92282b958577ecf94be74561700ca8f9625776ebf03c36f2292` |
| image | khoipd_indoor1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `62849b891e9bcefbd847db0a010c29ac7168557fc92d7660658fb242e2cf03c9` |
| image | khoipd_indoor1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `4a9128163e584acf6b6dd52ac7a5cb10ed5768a1e0217caf8d8ca0d705facc92` |
| image | khoipd_indoor1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `48e90815bd0f5b242460d914e378b2b4b8c6a9249fb215d4178f251cc4dd7e96` |
| image | khoipd_indoor2_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `c1367dd92963066a71bd49853a55025e536ee6b685516d685f98a464a33e27d1` |
| image | khoipd_indoor2_raw_2688x1520_5376 | full | 1920x1080 | 0 | `d6ddbeb988f1b8f503b8ce47ce29937570f0889b1ec111ca0ddcbdcb37b16526` |
| image | khoipd_indoor2_raw_2688x1520_5376 | full | 1920x1080 | 1 | `12b271c12ace3fe64e747bbf9b613c4a11ec59e4a6491c3a0d0b9ad1e5ac469e` |
| image | khoipd_indoor2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `ca070b11291404c2064c3f19787cf6aa971e6e49a6bce4095633675230f0c884` |
| image | khoipd_indoor2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `015b554168ee4a056ed5ef8261617b46bcc921267a5f7c96d0a215f80385ccd5` |
| image | khoipd_indoor2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `8531c3dd51e934c2d0e3d8541facb5f5238db179648c31e35b7c6c03c482e5f5` |
| image | khoipd_indoor2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `8531c3dd51e934c2d0e3d8541facb5f5238db179648c31e35b7c6c03c482e5f5` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `f256e44c2c9f8e95ea333da2417bd3b50a59a123ee86e42c05f3ee7a511aa51d` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | full | 1920x1080 | 0 | `fe6e4bb455e5aeef87862b02f80f9018cfbf5cb5356a047219425e773309b15a` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | full | 1920x1080 | 1 | `e5cc1eb222bbc99fd53079a3ade69447a0de5ef8afdc4b5986d39e7de363226e` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `be89111d6e2273c87da6e2d72c71bcc0e33f3bef05a81e647b4735ca727a52ce` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `d0d76ea23041f409be89b688d2e632ee5ea09771eb6f6396f3ba6032c965202c` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `c7d4e45de935f7aa5854fc40df776b38e60f20cd304dfc794ac17f6067771a11` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `42cf890300cab25d9b0ee22e9323581fc4d478541ea23b706e8d202279524798` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `29b6b1664b46b910f95bfab1e53068b07b0f15eb70d8b34dfc0f38fdd0c425f7` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | full | 1920x1080 | 0 | `812a49f402690de686899fbc4369d7c51f69639b4bf0b4911ab1da6aaa2c04d1` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | full | 1920x1080 | 1 | `1089f4b9ad8ecac2fc1d81c3e87c2dfea0daa588ffe12176ffe65b52649b4d19` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `59fae90bd50d11fdc0b29673d3fc5a78d70c87faedbe62ed20e8525f711babef` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `1e0af1d3dd537e2f598c950be8900cfa7d07f49307d0748b4734c5b16bb4746a` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `0baaa16eb70e81fad4168a625c650f5e12b469e372b178d9776985ca75ce50ff` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `c5e78fbe4b651a90f963f0992e117f9b561efb2eb1b3f066cfb885e77a101831` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `93845d0bc26776ee29d2bc0edb01c4c7ef09de3c0fad80efe04b60f802c9060d` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | full | 1920x1080 | 0 | `40341c6f305b203e1cd225e6550b460d4c8d93ff2f1364a1deb973899aeda632` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | full | 1920x1080 | 1 | `3efb26ecf9a3625433af3fc1c62ecf5f5296c4c4adbe49479b1a9ae9ae2e5e29` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `0bc4eca676787ce68d29f99d9ce7704c42237b5ae2715c266f2c700c5b32b793` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `0441f9514a3272e361f2607462a5f405f8615f0cbf42fe14c491588ec04133fc` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `0f30023c24aceffb52402dd5fb77e64611c3061f0eb241405ea2195d88cfd519` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `1cd2f9bc9eeee44fc6e1607d65c98c20164ccf27ee8b091bca4c573d353df387` |
| image | khoipd_indoor_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `a8e19ddb66ff4a393008fd46dea5bf67a3c1d39f7879e4aebfd96054c6444a8f` |
| image | khoipd_indoor_raw_2688x1520_5376 | full | 1920x1080 | 0 | `e624f0a29fae13637caa1566b230bb2c719a792950046d80957d500094a6b7b9` |
| image | khoipd_indoor_raw_2688x1520_5376 | full | 1920x1080 | 1 | `52edc062557b3526d582ed9ccd095b09bc553d02c13ad2990a20528024f7b771` |
| image | khoipd_indoor_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `27cc5a98740bf55325e0ff493e0c346a82c79dca67f5648cd0c384f2a3403b52` |
| image | khoipd_indoor_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `df01da6f5f95e4d3209d7412f2bdd2cb1ede40ad0c76aa20a4def05fdf4fc6d7` |
| image | khoipd_indoor_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `db3bee92e0704054b4bf9a0970044bb14e3d03468f85421ee56fcf4148046ac0` |
| image | khoipd_indoor_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `1044ba331f6d19ad3d2f7abcfa6bc8c7aba67d817c3342ee22be81b5e6fcd3bd` |
| image | khoipd_night1_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `06492195856ea3953862ede0c596c661a4341acdf92f949a5b78bb163cc87746` |
| image | khoipd_night1_raw_2688x1520_5376 | full | 1920x1080 | 0 | `18064bd3e6c6dc8e933561906121a5601cbb6e836c3bde2097605c0ae7e2e998` |
| image | khoipd_night1_raw_2688x1520_5376 | full | 1920x1080 | 1 | `6fd1603154ffd2a9e67737356bd057a669401ec90c069483907774a38c10cc9f` |
| image | khoipd_night1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `5d472db108b9df4e29b531271b5d7415268f709107d5c9efacccc98a435b10e1` |
| image | khoipd_night1_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `17247c860622707029bec1530215132b4afd0e2fbd72877845f4d92d9e686e37` |
| image | khoipd_night1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `8c1d4a7b43d6f0d1d856ef63a2306673c60ca653008b3da740206083644a7aab` |
| image | khoipd_night1_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `e6401c475b539f693c7f5f8927c25c3633e73713cc7e2f3716935c83f10d5ef9` |
| image | khoipd_night2_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `c0e015255d563c3f976d5cfb466a430300756fa7469d8d57fcc8dae98cb8e3af` |
| image | khoipd_night2_raw_2688x1520_5376 | full | 1920x1080 | 0 | `9aa113b89c33e87b2a5baf7153d9a83d87da46a872a14ba77509c36cb53ff68e` |
| image | khoipd_night2_raw_2688x1520_5376 | full | 1920x1080 | 1 | `60a553133300b7a9082d899fa9c4ef47950ffc0b1060e22eaabf7295bbf69d56` |
| image | khoipd_night2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `03526a6b4ffc26e982a7445b7710d695f65a5ec50dea166f3dcb312216720b4c` |
| image | khoipd_night2_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `04613c3143216295e749562d750259c2b8862db08fe3de035241a4fe320e4145` |
| image | khoipd_night2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `4cf622e2a919d34d4b5165ff3c6e9e17eba12eeb01461e2201e82354d7d67f3a` |
| image | khoipd_night2_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `664d9347c39bdec8af760725239ae4103cf47b47240a3f2b748e5bf17259bac6` |
| image | khoipd_night3_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `7bafac4395d12bb69559bf10b83be6b1863a03436dc9cda8bedf2aac1fe73c3b` |
| image | khoipd_night3_raw_2688x1520_5376 | full | 1920x1080 | 0 | `54bad710e40f6e42a8afb67dda0aee81d4aed45ef16688424ebb1e956a94f5a4` |
| image | khoipd_night3_raw_2688x1520_5376 | full | 1920x1080 | 1 | `cc5d1276f1134c2cfefbd990d9024b20ec4b889573facb3c7ba50b93f1b5ec20` |
| image | khoipd_night3_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `060b5590bb2a9d4755c53edc246ec4f5b57ce49c300d0b4f0a01873c07cc9085` |
| image | khoipd_night3_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `bea3ebfcda2feb6ed76d665cafb88d5f7b447a1b777d1502401a702a613a3bab` |
| image | khoipd_night3_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `70aff0bb4971f58d5ff830e96335116a941c28067bf91d23f70f6d3661131ee4` |
| image | khoipd_night3_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `97c10bdc97b6e70817591de100c74ac6beee4e67d8682eca7fa116d0df04fcbe` |
| image | khoipd_night4_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `aaa9d8696fa35c888cc70d5ab9e2927b5ee049c93d5ff0f9cb919b14885fb3d7` |
| image | khoipd_night4_raw_2688x1520_5376 | full | 1920x1080 | 0 | `e7e59effaff9dd82b85f0b6c7b20c8e99e7e40363be24adc7da3683b6a741bab` |
| image | khoipd_night4_raw_2688x1520_5376 | full | 1920x1080 | 1 | `3e28e45e4a56d11b2ac515b765975d93e4df9d4e5ebbeaa7767b9453a5c49e8e` |
| image | khoipd_night4_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `b8bee6dc26c9edc6a9b9e8a88a0c06a5183aeb70574cacfa8c2ecb9538fa1b4c` |
| image | khoipd_night4_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `c8a28efd22ce8fac5dd4ca94ccde08e7b9163083cdfa9cafa2e4688248eafdbd` |
| image | khoipd_night4_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `82db938593f6bfcdc2a6e4319958cda1fa344978680eef10adfb670fa9c24b21` |
| image | khoipd_night4_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `25cd35957b899bc0a2967dc6a19767679dd3a7a0e700f63c59fd440878cbbcd6` |
| image | khoipd_night5_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `edbc52e63597dabb5935b4b040b7a2b4ddff86d55d18177d5b7bdd701e4be1bb` |
| image | khoipd_night5_raw_2688x1520_5376 | full | 1920x1080 | 0 | `9f87d9918e97aab454101e4b0830f5f650073298e1f011b5f688a422f4be8c07` |
| image | khoipd_night5_raw_2688x1520_5376 | full | 1920x1080 | 1 | `257dc6bc884975739d967aa50210342631d291f0d93bbf5916ef6a7d901b0c2f` |
| image | khoipd_night5_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `f5f31fb9503dd9a3b58755311936e9b8071defbd245d5c3ecbcf0c38146cb7fc` |
| image | khoipd_night5_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `c0ba24be744008bf1928da6566f2d761bfe14d0a017d9f6b0a3c52dce51bf754` |
| image | khoipd_night5_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `a05a09c6c0d01a06156a582a4eceff67e072ac00b7e3352a74ffffa42654a930` |
| image | khoipd_night5_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `1164530a2b8c3b96257b6e36413d9566793bf72b2509c1162534d2b56bc9e00d` |
| image | khoipd_night6_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `ffcd389e1bdf93945951bb5ac31b37c131017fbb73c2f816c5d5e88baaa1a241` |
| image | khoipd_night6_raw_2688x1520_5376 | full | 1920x1080 | 0 | `458893517fd98f34c82ff858bb4e9502ac65c1cf4fbe03072bffd3d796eb2a6f` |
| image | khoipd_night6_raw_2688x1520_5376 | full | 1920x1080 | 1 | `bb8c39a1d1613c99de441dec3960b4bf42e06c5d526e1de09510482bf722257a` |
| image | khoipd_night6_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `6ad9692e769456b28ba70f92e937aa6470772384bf4ee515be602a7d2d5c8a69` |
| image | khoipd_night6_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `a955426ef19c62b3ba4cb63eaf6aba517fe74cf4ee934846f41a21a7c2142468` |
| image | khoipd_night6_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `d58671745be609e53691b9f1be738743bacf7fcc50de4171469f20708f53da14` |
| image | khoipd_night6_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `b6e4badcc40f4292db014f5d3b76acefe962b357cbe3ae7d59b181acb78a7b5e` |
| image | khoipd_night_raw_2688x1520_5376 | basic | 2686x1518 | 0 | `4a68adda77fd93f37465e54eae89c0759302ee1963926eb53cb0130790757863` |
| image | khoipd_night_raw_2688x1520_5376 | full | 1920x1080 | 0 | `b97d326e9dd794ad3b9e74950b5ee05f7b38d2632bb9505ffc7a80e9987c5bc5` |
| image | khoipd_night_raw_2688x1520_5376 | full | 1920x1080 | 1 | `0f3177fe3e1781172fbd7b90b4ff437ec5f9edc6888d7f50086b00c46a18f092` |
| image | khoipd_night_raw_2688x1520_5376 | full_vga_709 | 640x480 | 0 | `3ed77d9eb55e726bb789e905c5cf35f13a123ab12a972b57f1455c740ed4c617` |
| image | khoipd_night_raw_2688x1520_5376 | full_vga_709 | 640x480 | 1 | `627c7a1035eabd24c7a7ade33b0ac63e8b39ecfb4f11fcbffbb5e7578ff56c8f` |
| image | khoipd_night_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 0 | `1eca81f3be32b4a5467ff59654f2c4c39b888b88f709f6d6e6b867dce92c01fc` |
| image | khoipd_night_raw_2688x1520_5376 | gtm_manual_qhd | 960x540 | 1 | `a14c56b64ec7808ac5573def419a1c62a4e5123e985d75a4c57c7e8015b63441` |
| sequence | sequence of 28 | basic | 2686x1518 | 0 | `f1cdc84f9b73b1f0ea500a43bb29cbf053250728d7a85eb9f021a2c2f8ae031f` |
| sequence | sequence of 28 | basic | 2686x1518 | 1 | `9c54ee8a37755e499c9376b4a0b7125ecf7b66a1005a3816f3ef401730b5fa7a` |
| sequence | sequence of 28 | basic | 2686x1518 | 2 | `acf5a5116c6bf217ecb31e424432ecaf08afe0e6da4023c579481ff0882bed3f` |
| sequence | sequence of 28 | basic | 2686x1518 | 3 | `dfa49ddb38857c5f74a6f5388888bc6522d37182147dd151fb0a8de2fc55c75a` |
| sequence | sequence of 28 | basic | 2686x1518 | 4 | `0ce336373fd8d5fdb7697d5d75ed730bd4b04f79aaf22dc682a1cfbea3e614af` |
| sequence | sequence of 28 | basic | 2686x1518 | 5 | `41270027c6aee0e63a71f8f1f51249c41fad93f7f80343d1243322c95adfcca3` |
| sequence | sequence of 28 | basic | 2686x1518 | 6 | `e8f5332e63062c9e395d4edb40f65d8fadb3329af899a70839a354cce73302ad` |
| sequence | sequence of 28 | basic | 2686x1518 | 7 | `19048cf19926e494942019fb3e3dd51d356ba7901469c3012af0d903858e02f4` |
| sequence | sequence of 28 | basic | 2686x1518 | 8 | `5501a46539d12c7be4ccb59071e0929607ac3805b63260bb997ea3e101287134` |
| sequence | sequence of 28 | basic | 2686x1518 | 9 | `f2de8d8d0c0aabd117d86b527a7a4c7bfc2ec29e10a9db56c203b43c28a389f5` |
| sequence | sequence of 28 | basic | 2686x1518 | 10 | `c0e61a79e9f357d3d07f0c2a76660b2fd8bbf63ae0fe1752dca73a63816999c3` |
| sequence | sequence of 28 | basic | 2686x1518 | 11 | `e546b94621bb2d55f8fad08bb6b95316fd1d509a7d75b35580fc55d461c51c0f` |
| sequence | sequence of 28 | basic | 2686x1518 | 12 | `d7eb4b88ed3a5378d1009b37e39d07a24a0c05792882b90f154c763fab05801f` |
| sequence | sequence of 28 | basic | 2686x1518 | 13 | `b9be84dee37fa43fe3eb16a5c05d2cbf42c4d3fd0990048710553c5829783152` |
| sequence | sequence of 28 | basic | 2686x1518 | 14 | `247aa9b73a61fe54aa17f98ae64b2b60b1b8ae7d38882cb133f0a8c28d3faf1e` |
| sequence | sequence of 28 | basic | 2686x1518 | 15 | `82565d896873bf28c77f2b93fcfd71612ee92912dce70876f6c4513d27ba2f6c` |
| sequence | sequence of 28 | basic | 2686x1518 | 16 | `1fd6da1adc87012d2566ce7b9442902cda3fb3b1b17da3e59ce37cac784fd089` |
| sequence | sequence of 28 | basic | 2686x1518 | 17 | `cf6e15a8ef38daa0c291999346fb8b5374c8fbb3d3af4cabd538c98b7a60e46e` |
| sequence | sequence of 28 | basic | 2686x1518 | 18 | `7aa8e2fd6a878b72efe93deb114b1e78ed605bfa9e7f137ca03da81dcd25f3df` |
| sequence | sequence of 28 | basic | 2686x1518 | 19 | `9159ba9c32920cb3a77ec6d85b68c440e877a3c5a3684d8cd52e8ce8ee295f3f` |
| sequence | sequence of 28 | basic | 2686x1518 | 20 | `dcd19240d5ea38582007dc48deb9ea8d349bd5a54c9ef0046ba0a06199dc19c5` |
| sequence | sequence of 28 | basic | 2686x1518 | 21 | `bc7abde66ea044e1c6ba9c996320b3a466a1060c207604f679f942bf4a5f5838` |
| sequence | sequence of 28 | basic | 2686x1518 | 22 | `d609c00ef75c6aea25d05b1e49e3c2cf1eed977d43c66a18fd938832cd3f230b` |
| sequence | sequence of 28 | basic | 2686x1518 | 23 | `9b1b2dda45198a414309a1bf197145f24e85323626be53468008d1c36e28ab94` |
| sequence | sequence of 28 | basic | 2686x1518 | 24 | `442378be4a1e0bfe62458e2df7e3392a7e2025860b00f96abd05239f78b3afd4` |
| sequence | sequence of 28 | basic | 2686x1518 | 25 | `69e057cb139ae03a0e6f7a1c46b90db95efd3833cb46f352ad011f93487a5de2` |
| sequence | sequence of 28 | basic | 2686x1518 | 26 | `635e4cccff8f2dd5ee61317c1ebe92868a9043cf414e151ad96e7857751ab211` |
| sequence | sequence of 28 | basic | 2686x1518 | 27 | `ca18efa337d01d41e96893e1848edcfbec7728738f326f7ed8b429cb0c415f89` |
| sequence | sequence of 28 | full | 1920x1080 | 0 | `0cdf1258539ea2047761551f12cbb4148bae28b1c1043f57df9928761199ac36` |
| sequence | sequence of 28 | full | 1920x1080 | 1 | `82a26db0f236b98beb1f606be73ecfe2eddd43fb5fe04a19f0adbe182202fcc3` |
| sequence | sequence of 28 | full | 1920x1080 | 2 | `780503dfdd09f4140b838ddc1ab8bac9afbdaf496607b572f5060b154ffed67b` |
| sequence | sequence of 28 | full | 1920x1080 | 3 | `e29b7b6bbb345ae05a374d9f07a95d8c21bfaf87d5233ba52672ec46bfe3d33e` |
| sequence | sequence of 28 | full | 1920x1080 | 4 | `32a24c2671972161bdf705902ea6764d4105aa0a82e67784faf0371d8e694d14` |
| sequence | sequence of 28 | full | 1920x1080 | 5 | `aa828a4b2ad3b8d9228cff34dc970a7d37993f317a0bbdb17ad63b9b9fc0a93e` |
| sequence | sequence of 28 | full | 1920x1080 | 6 | `b1a9954f3d65c100289aa9b15ca06ef2c247c8874f3d1437ebf35e4826965863` |
| sequence | sequence of 28 | full | 1920x1080 | 7 | `bdf2b0cd455792512542e82fcf9b40dae5a31222e4e43df92bc8eff2bb4329fa` |
| sequence | sequence of 28 | full | 1920x1080 | 8 | `57cb64b00bea22264a7fd6d6b2c50936ee8c740b68cbdb3591b3b9418ed0df56` |
| sequence | sequence of 28 | full | 1920x1080 | 9 | `26c88926432c423197438b825e2856f11aa4f94fed5538f0095cfa87b3338c59` |
| sequence | sequence of 28 | full | 1920x1080 | 10 | `e51523d8a337bd9e690471f4fa431f95d23ddfbd7215a0b0c39a2d70c8121503` |
| sequence | sequence of 28 | full | 1920x1080 | 11 | `76c94113db2ff039e36820eb660f1d8eda6871d8206431393b23a434af1f10dc` |
| sequence | sequence of 28 | full | 1920x1080 | 12 | `3cecdafe7d29621b36e35ca20b5bc4b8b6ea63f2fcc48fd055dbe48d3b60a537` |
| sequence | sequence of 28 | full | 1920x1080 | 13 | `255cd0ef208f52b051d2316a32eb8452b1f33aabf2fda1a5893c1299b4515a4c` |
| sequence | sequence of 28 | full | 1920x1080 | 14 | `520cb06f04cd4e9fe25ab4989466096332d7e78e5740a06a689b7a9079173878` |
| sequence | sequence of 28 | full | 1920x1080 | 15 | `e720336060402741f8d5fbbe491d4343df3d8afc533854426fe164e652e52184` |
| sequence | sequence of 28 | full | 1920x1080 | 16 | `c40a32c96971837c9782075bae031e2846f9499d76e8f18d2c50461a3dcf5ce4` |
| sequence | sequence of 28 | full | 1920x1080 | 17 | `97bbfc92592e244520548d96dfe47ff78d960ae9a32b42099bb1c943ff590b28` |
| sequence | sequence of 28 | full | 1920x1080 | 18 | `3a49ef4e0650d32506e7d563762889923eda5dc20588be60168df3060a89bdc5` |
| sequence | sequence of 28 | full | 1920x1080 | 19 | `cb1a67e6126a30e6046a5352cf437ab03bf2aea1cacb95e474c1d6752b16d8dc` |
| sequence | sequence of 28 | full | 1920x1080 | 20 | `574934e5b1a027faa9809319627ac7e63d93c272e6836d47b971dbac7345c81c` |
| sequence | sequence of 28 | full | 1920x1080 | 21 | `71621c1c4e7a8519a33ecf8dbf14f0e1ac30c45cdaf5292f16a08ec73ccada55` |
| sequence | sequence of 28 | full | 1920x1080 | 22 | `e9ed3a227de1cc2e8f28d983910c2408fbcdb60cb36c4ef58fb92dbf889ada1b` |
| sequence | sequence of 28 | full | 1920x1080 | 23 | `530d2bfb0d4698ab6d29652d008674a86dc4f9af5e997cef1b68cc4bc8eb5ad7` |
| sequence | sequence of 28 | full | 1920x1080 | 24 | `24394c312210dd66989fd64b68538f1b90a2f427593e33bb7dea5311d4c6addd` |
| sequence | sequence of 28 | full | 1920x1080 | 25 | `9e59d1ab7b5dfd5abe7db8f7bb346e64b5ddcfa45260bbdf590bc57b5f4fc537` |
| sequence | sequence of 28 | full | 1920x1080 | 26 | `9a51aa24c2763efd8cd0cd8c47f421426e0408d2b52c294056bdd9c2a3d20db1` |
| sequence | sequence of 28 | full | 1920x1080 | 27 | `07087cdddb69f3a22485b0b8ea1a53cfb947ce353c2ae280573b87e81c145799` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 0 | `d95807b504e8d0e201f9f478fa5a6779118f831a5eb2e1fc61b685bd2d643369` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 1 | `4c2b1d4da07e7005adbce34da2f40f570470af1535b0308c30fff694ed296572` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 2 | `517135f757d9ff7edd0499a787df73873c85d85d1b91c3ba62cfe3112e999978` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 3 | `bd3cb1377421a5a8d3b20394280e44855f19d0de7c50e60dc0acd04533d28aa4` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 4 | `fae9613140b3e67fad21268f3baf4ebf58ca84e7e2cfb05554c270bf9a5e59ae` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 5 | `a9d83638951c0b54cb1d06e7c1752e43a08c1eb79555461df2d78cff49f99b42` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 6 | `c7f1dd795484afea07f54d8e67c025c195bc2fcff785a074345c508e961bce23` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 7 | `dceb6386f20070391b662be92e8c7703ce7bfb229f2286995159c717c73c43e1` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 8 | `4084fa03f56a27178c7646eca5f294d8e2d7c98cfce37b68f1fa573e5e0e5d78` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 9 | `87ab737825972f11742bef4a8369292b435cfc3ceda59940c9e861d3ea5f49bc` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 10 | `1976ba720456aa48ba60ba55c334dbf658a8848756d92d65bcc4c7579b3ea1b2` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 11 | `ab7ef3108e8fbedfc33ae111e3a7a0a0e4cbacc11fb27fc8e5bb1c378dde97ab` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 12 | `80a039536560aa742afc4704154df480fb2dcca4af48a33d958f603bad0163ad` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 13 | `a31d1070359485a154724d5a055df177dce1edaf8c96da04028e35a66d0610aa` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 14 | `fe734b7af2dc5f03f0938426e4f477651d07fd701950069d1b6833f1a1f849cf` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 15 | `ad44d9e72c9a4413d2ec036298024b043cc04c621fd1ed1b12451aa670c1dea5` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 16 | `7dacc5e45c7af993bda988a259d167067f4a4889005c938de0db5671cb11456f` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 17 | `33f213e4106d8ad517f4b5a81189f75471f569c1b7a4bde7f6d2541eaa0463b6` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 18 | `073bcca2fae9dc1eb241f068d4d78c338bdf4c8edf545ccc53a1ba3ded1d2de3` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 19 | `9d5d6b11b4970c122f90e3f85456be111cd2f3d8ecad61bae3d61d91121ff8e7` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 20 | `2b85e299e81b3d27e3cd4b49ac767e0f50ffc8f7d54dd29bbf0714f8c739547a` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 21 | `19aeb17ef3e18dc64e97dd21424f7bf3b8712bfc8f0f459221f2f0650a2efa7a` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 22 | `cf36b4df7526521e069bd5686b4a9c3b62638f0c226488e55e1427dc71606dfd` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 23 | `67cf0b5506a301025d6040be7e6c3774ff0de5bfba5817f2fd2493d67d9c70bb` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 24 | `18483a1ae07057f534d7f0679cbb4801a0ebd56dfefb20074f6bec1177144858` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 25 | `192a66e445c89d7bddb65c30fe2da9338276feb1be04c1349dd6a771c7d6ad44` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 26 | `2580f702ae2dc267bbc8ab68850f3b96be3b6e57a50485fde15df753d8987e74` |
| sequence | sequence of 28 | full_vga_709 | 640x480 | 27 | `30c09313746a5b26d18dfda05427bc1db7c71e47cb2ff9e7a15803801f2ff7aa` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 0 | `5dfd67cc1e672a011c64b3106c8b2df7f6e98e10dc7ca770785705abfd430620` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 1 | `121ca586fd27b105043a55bd04c299f11619cef50025c0b71390991b6d40848f` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 2 | `628cfbcaf4cd0a90c87d3f4c18a212a4d10876ca89162e0d4f6810c4519047eb` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 3 | `6e850754caaf8e33b008d1c94520dccfd4383ea400b427c448df9b24854eadfa` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 4 | `7510a750303e965f27e8357210f04636ce80c7a7696ce195f541b6b4b3ee0d44` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 5 | `7d5dc745a9b017c82ccc6646015f72f3661034d85d02d3dbec4c6f16fb8bbcb9` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 6 | `e45e251d6cf6c157fb6d98cd0f71a9902d75d6a257b6754babc6f1a1a10cb1fb` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 7 | `cd62d86bb43ec4df53e69b70809947f0251b60c50fe1d3a6ec29f25d3debc780` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 8 | `02779db21e08d689b3ae142d4ea3062d0ef8988400dcdde2a2d9b0a7464181c9` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 9 | `5a391318ad94292e36fa438f95a8992059cb8956280ae612355a485b8194cb77` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 10 | `6ff27d9ed406ed3b1f70f41f0c5d0d2f01b4b4e993789f787d244da99a817278` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 11 | `6e3c41a5551e89f25452aac84a3736b933d30abc655250100ea4fe02b5006f1e` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 12 | `ee425517668360b8e3a2af515dd2aee354fff20a3faa789e35bfe00bad68383a` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 13 | `7864c505680b2ce5f8fa134a209391b48b13061b5f025744e8904b0055001722` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 14 | `b5f0d59ccc990fb15337384f77096dc68691be499c87d8d18ab55a5ae29ef2f6` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 15 | `00b17de5685c52a15e2b63093c286d69942094f28fe20a2d20a67bfc13119644` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 16 | `afb183c43624b65ba726498ca3737952cf47f7c9757e9dea02cd5dd6f2e66ea8` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 17 | `952aec6f3409d623822afbec6ddeec39d2490598c891bc5dd8d78eb25310697a` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 18 | `532d20817b8ec016ec1a198195c98fc9ba0b25f965da3c963e3f6c37c0413664` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 19 | `ddf8ef3f94724cea229a4f54c51d29afcbbc5bd59dd52991aa20a036a80f1625` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 20 | `91e777e6db782483909b5cd314504989c558b1dcad28fcddc7d1d8020d400a37` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 21 | `6f7e57a1fd946f5e57886c2fff21a6b16db4d2531c1441f5437b1316e90bcd1a` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 22 | `572724ae7d713a57297cf27b2eec4f26e1c10d58e324b4e31135b836eb90a74c` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 23 | `b57f5c40a3f1964d762e2c37b040db2b4e608cef2343efab28f28585df469681` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 24 | `b0088257623c25341cbe405d5673fd18627674db41f063660cb318ad8faf74ff` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 25 | `05e82a4fe160fc0216674125bbedf4e3a66d27af9e23151d65ceb03fbf923879` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 26 | `4c30a79a9cdbed77f53fdc6cf9783725f05f21bb52af064b8ec3a27fdd7ac74b` |
| sequence | sequence of 28 | gtm_manual_qhd | 960x540 | 27 | `5e63039c57aaa3d6635524ac412d6f9d31df44f65faf2a1a8dcae0f051c0f8ab` |

Statistics readout SHA-256 (after the last frame):

| kind | RAW | preset | SHA-256 |
|---|---|---|---|
| image | khoid_ground_1_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_ground_1_raw_2688x1520_5376 | full | `7aed515445e7d2610bdb629cac31e9d2d23e8f0820483b45d459f55f32ecf4f7` |
| image | khoid_ground_1_raw_2688x1520_5376 | full_vga_709 | `9f842397bb41b666364f857a89d13e9f4ee024bb0b88f9411fd38af1c7a28b4d` |
| image | khoid_ground_1_raw_2688x1520_5376 | gtm_manual_qhd | `67afd9287b23b8e632152586263abce7c8a1b4161f73f8e060023953f574074b` |
| image | khoid_ground_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_ground_raw_2688x1520_5376 | full | `6ed5d03b154d6eebdf6c77e331142af810c0504dc71fa26112fd12f57aef1145` |
| image | khoid_ground_raw_2688x1520_5376 | full_vga_709 | `2f2597191b13dfd1e973c186299afb3cfc8862a3a4656c15835ab50b58771a3f` |
| image | khoid_ground_raw_2688x1520_5376 | gtm_manual_qhd | `d7dd8e497148697e0a42e27a0ae1852949524492b165c97d612e22a9bfeabbb6` |
| image | khoid_indoor_led_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_indoor_led_raw_2688x1520_5376 | full | `1d87706e933ccfe27dd29340d6f852a367e2620763ffa1c0ce0c4458e2a5343d` |
| image | khoid_indoor_led_raw_2688x1520_5376 | full_vga_709 | `2c2b54f3f233e329c4e2c91d31b54acf71cc7aa1964b4b11065ab3fa12239bb7` |
| image | khoid_indoor_led_raw_2688x1520_5376 | gtm_manual_qhd | `46f4c904701eaee2f5c6d0cbad7c6f72da7e5407ce97147b27ed212d510d442c` |
| image | khoid_outdoor1_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor1_raw_2688x1520_5376 | full | `f4007b40bbf9826736808c7136a2a2d685340c0e55339505d5d5ed012725a490` |
| image | khoid_outdoor1_raw_2688x1520_5376 | full_vga_709 | `4c6e3486006eae5a0c629d96adbba18e227e86940ad31df9d37d9f9a14f32554` |
| image | khoid_outdoor1_raw_2688x1520_5376 | gtm_manual_qhd | `2eddd00f19434b788a224e3f49246e52f0096c78576b0385a7b9e068a4f6459d` |
| image | khoid_outdoor2_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor2_raw_2688x1520_5376 | full | `dfe42eec59bc3bc7b4df3695488f1695fa233ad6e9ec63dad686a1f162388da7` |
| image | khoid_outdoor2_raw_2688x1520_5376 | full_vga_709 | `25e9f8a4810824666d3316eb7dd06f9ea1487d11f7297d2f799e6a0824c4e213` |
| image | khoid_outdoor2_raw_2688x1520_5376 | gtm_manual_qhd | `0e954a22e5681d25f2b094b92d26b8be119c776328f9d234e3263cafbdabcb29` |
| image | khoid_outdoor3_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor3_raw_2688x1520_5376 | full | `90fbe95bf73f9cc34289369a94a39cf9c46faf4091edd8af29f23d3f86049da7` |
| image | khoid_outdoor3_raw_2688x1520_5376 | full_vga_709 | `57faec8b2e047a37cc35c933dc0e4a07eafe3a98a27450a8b39a927782332d5e` |
| image | khoid_outdoor3_raw_2688x1520_5376 | gtm_manual_qhd | `12194ba4ec7fde6645ec6f7caff3785de51931abd6b8fea6c44f817e54becf21` |
| image | khoid_outdoor4_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor4_raw_2688x1520_5376 | full | `98ff0669fc5343ea0e2dcf45651aab8e17f9a1c9c7f47f084bbe0326f72e5370` |
| image | khoid_outdoor4_raw_2688x1520_5376 | full_vga_709 | `bf02d17da0ada004107af6df9039bdda12651ca2dfadf3b030e766ec2ec89623` |
| image | khoid_outdoor4_raw_2688x1520_5376 | gtm_manual_qhd | `e73768b5202e2818dd5b636fa3dc83a517bec53c58c812de42dc39a2fa10820b` |
| image | khoid_outdoor5_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor5_raw_2688x1520_5376 | full | `812f389e10c15b37289da37c4c2e4d29e2cf348314d57aadb3f6003338e25261` |
| image | khoid_outdoor5_raw_2688x1520_5376 | full_vga_709 | `7b6d68663731b39a5d809d337743fca498c09892acf9f85c0e4f7c6394bd0b37` |
| image | khoid_outdoor5_raw_2688x1520_5376 | gtm_manual_qhd | `ca9d4d2181da006657fcf182c653583632079a5ca916deb024d2e58325294d57` |
| image | khoid_outdoor6_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor6_raw_2688x1520_5376 | full | `d248ff6c0c099bfed2d7afed50b50fb1719f457d487c1efba634fe6d8b9fd31e` |
| image | khoid_outdoor6_raw_2688x1520_5376 | full_vga_709 | `f771ee89e76572c9fd1bf5eec837312fd97fd1cb764ce285af0ee90314551d56` |
| image | khoid_outdoor6_raw_2688x1520_5376 | gtm_manual_qhd | `525fa3bfde3b45b90f9bcc92352fa0e25aa7123db1f3f76b14f7a914ddf7ecf7` |
| image | khoid_outdoor7_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor7_raw_2688x1520_5376 | full | `db3ff15d7e32f27859e08942c1043c5356a0ad482d012f922b82d99fe18cc724` |
| image | khoid_outdoor7_raw_2688x1520_5376 | full_vga_709 | `c18897f79a7c450f7fc8373f74ca267e4606c39e4253b6d9861f7b96b5a9d72f` |
| image | khoid_outdoor7_raw_2688x1520_5376 | gtm_manual_qhd | `a95549f4317efed211fddfce107b9f1fa356572e668c95331c147acfa11b7106` |
| image | khoid_outdoor8_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor8_raw_2688x1520_5376 | full | `6efbda964c126a02360b08f5a85bf47f5fefe752d5ee6aa1cac3192d5503ab33` |
| image | khoid_outdoor8_raw_2688x1520_5376 | full_vga_709 | `7750f6315dac0499bf6bbe7902b80562aceafbde8f3047a147607433ce00f750` |
| image | khoid_outdoor8_raw_2688x1520_5376 | gtm_manual_qhd | `f7445e090efe3e232b074eb0b0bd8facb1c79c0cb430c67603324efe374cde15` |
| image | khoid_outdoor_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoid_outdoor_raw_2688x1520_5376 | full | `c2fe3f6f533dec3415c8915df881824d96b507c0d03b481422269fa7de85b5e1` |
| image | khoid_outdoor_raw_2688x1520_5376 | full_vga_709 | `78a24937e134297d37162fcb79d02d47714a2aa938c916d96aa531061fbf8436` |
| image | khoid_outdoor_raw_2688x1520_5376 | gtm_manual_qhd | `8c593ae05d01c4bd0e59ad7e1ae86a304629478ba30a7f9a4d28d54a5f1c2eb2` |
| image | khoipd_basement1_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_basement1_raw_2688x1520_5376 | full | `83f73294c6bede0794a5b66f1dc605e6b8efe9f66ad29b99bec816a0cda0f70c` |
| image | khoipd_basement1_raw_2688x1520_5376 | full_vga_709 | `44ec8dcb1b8ca711de6d682b3924a66208c0f90486bab82d90165ce528ffac04` |
| image | khoipd_basement1_raw_2688x1520_5376 | gtm_manual_qhd | `32d4bb8302e18a2a6484ee9671cacae748b8e2b53984f6a16046f9bdfc5b3bfa` |
| image | khoipd_basement2_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_basement2_raw_2688x1520_5376 | full | `77d02ed110b3a5f62db94c32eb4e3c98de210c90636ffeefbd4c6268cfd52e63` |
| image | khoipd_basement2_raw_2688x1520_5376 | full_vga_709 | `6e4044f11c8cffc229f77b5ab3f3fd18480d388706ac4fa93063ea6cc55117c1` |
| image | khoipd_basement2_raw_2688x1520_5376 | gtm_manual_qhd | `39981cc5703ca209f6a138a7dc97d68ad935a66ab0d056cfafd9455b7e2c2410` |
| image | khoipd_basement_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_basement_raw_2688x1520_5376 | full | `5a3d3f013737cb789a01b388027935e9c95e4fe6acbfa867c6e397b6745f9672` |
| image | khoipd_basement_raw_2688x1520_5376 | full_vga_709 | `95caa43eb24de67f41a311569e34d320b20491e686c6dab77e37d24eca78d0be` |
| image | khoipd_basement_raw_2688x1520_5376 | gtm_manual_qhd | `5637edb329557fff22904a19e785fcee015171c3991b87ead239ae54778cb3da` |
| image | khoipd_indoor1_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_indoor1_raw_2688x1520_5376 | full | `49851de6d43a44b3c02285fbb23f0d8a86583faa91f96835fa209df57b6a2bae` |
| image | khoipd_indoor1_raw_2688x1520_5376 | full_vga_709 | `74e013569da01fb57d2c77616d6b7736eb376546abf8596456e80a7315799de4` |
| image | khoipd_indoor1_raw_2688x1520_5376 | gtm_manual_qhd | `f88448208fb2e252bdd928cd077784b2c8b6053a025f1016a4b2aa2077f20e18` |
| image | khoipd_indoor2_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_indoor2_raw_2688x1520_5376 | full | `f08c12a3cbf9160601c165a49042b8f14be06cc475b44746a88d871162a4465c` |
| image | khoipd_indoor2_raw_2688x1520_5376 | full_vga_709 | `f6ef10dc8e65bdf40a03063c1c3386a792262a80052b5027fac87336c6620c01` |
| image | khoipd_indoor2_raw_2688x1520_5376 | gtm_manual_qhd | `666802aff9aa6b054f522b9b62f6bd8540ab79de1038fb0150539d8bc4f6b5eb` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | full | `3703f500921ed2b8b5ac313784c06c636f1dea142c04de017c58509f6b66a469` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | full_vga_709 | `ffc99304c20a6fa8ce6508b53c8d2539bd06c982627fe0fafd6ada08c0c14e40` |
| image | khoipd_indoor_figure1_raw_2688x1520_5376 | gtm_manual_qhd | `b68f9ece7b892103bec9bdc7a904fd216156364f04a3798d14f8ab9e78b87418` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | full | `248b2519641b6c6b3de594e0984c9dce4ccd67c569fcc560179f040968910a01` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | full_vga_709 | `d12a59ec06b811d2a4091401b31076501f94093b68cddc440201ca68df0b030e` |
| image | khoipd_indoor_figure2_raw_2688x1520_5376 | gtm_manual_qhd | `b8445d8fb713d5d7f73d6f9a813f6586aca368a57caa7cfd3c54d1a005ed5d85` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | full | `9f6f558a0fb41f3531d57630468a63e2b117ac30ddb8f20f3893384a69aefc07` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | full_vga_709 | `c4242d3aa1bfe28f0a7a3803e9ad3dd5478914436b7a3ddc50b90e00db68e62b` |
| image | khoipd_indoor_figure_raw_2688x1520_5376 | gtm_manual_qhd | `241b5a958cb9bcce299de8b1dadc4f4e9fc62baa8f69983ef432bea7a36f0ac0` |
| image | khoipd_indoor_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_indoor_raw_2688x1520_5376 | full | `34b81ce85601b991a6fbc305739582afade2028975c14d9655d2eaa7203642f3` |
| image | khoipd_indoor_raw_2688x1520_5376 | full_vga_709 | `1de84cc3a9fd2a3ac60cffa844d8460737237dfc84d92ab542ca17ec9db1919d` |
| image | khoipd_indoor_raw_2688x1520_5376 | gtm_manual_qhd | `c7818784fe6f26f344f7228ea832d1349b33d5e8db3cc6372210b43b4244e336` |
| image | khoipd_night1_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night1_raw_2688x1520_5376 | full | `405f706ec64524400f5f113e7a7e3deea7f9fb426de3d5ca6b546b1acb9964a9` |
| image | khoipd_night1_raw_2688x1520_5376 | full_vga_709 | `199864770c9b3eb32df828a18050170ba92df976a6b1af67db8135d685700b49` |
| image | khoipd_night1_raw_2688x1520_5376 | gtm_manual_qhd | `2783738c649ed568b4d8502ae9102ffb142c2431a21e4428478efc2581c0740f` |
| image | khoipd_night2_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night2_raw_2688x1520_5376 | full | `3c33210bbc746f871819890e058c48ae8870354bf8df02af53d7fffbdbee93bc` |
| image | khoipd_night2_raw_2688x1520_5376 | full_vga_709 | `9724982721bce3c3642e9437ae844fdf8377b681d167dc9f73534d5d93249990` |
| image | khoipd_night2_raw_2688x1520_5376 | gtm_manual_qhd | `510c2ec6d97749459c23a69b2bcc5c4465448a19a0510af03c278f20bb80de19` |
| image | khoipd_night3_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night3_raw_2688x1520_5376 | full | `f9a16d617e7848e832c10214da9fed4cd4d3bdb342f7fc303ce2c7f437cddc1e` |
| image | khoipd_night3_raw_2688x1520_5376 | full_vga_709 | `9528a6fcd3c17841132df6adb2c6f58ee7ab0f741642180cb00ef43988dbe336` |
| image | khoipd_night3_raw_2688x1520_5376 | gtm_manual_qhd | `49b108ed480d8fd68420c70b302a006ed108178fa8f995420622f5c1f024d4be` |
| image | khoipd_night4_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night4_raw_2688x1520_5376 | full | `33ffbc9224fe78d14f5a744e4ead46ec34488b9a7f6c3a0deaffed09e816ee6a` |
| image | khoipd_night4_raw_2688x1520_5376 | full_vga_709 | `170653072b5ef94cf469be7fe5ea4f5e7aafa976b538b1e1bb8f181eee0d1529` |
| image | khoipd_night4_raw_2688x1520_5376 | gtm_manual_qhd | `fa14297b571d70562e879f004ee64800a139fa03c4f2d5644a331a4bd6667e9a` |
| image | khoipd_night5_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night5_raw_2688x1520_5376 | full | `d77001dd01920a9d8ed607db073c0edeaceda0c79fce65efda37e4f903cb7a46` |
| image | khoipd_night5_raw_2688x1520_5376 | full_vga_709 | `268fbc09c1197fa3a1994787cfb4e853de9a29cf853d72a12f5d5a22e95a7cde` |
| image | khoipd_night5_raw_2688x1520_5376 | gtm_manual_qhd | `1a17a881121a0e88117ea044210e474f50a1e020f7adc63e8354132e5f7f1af6` |
| image | khoipd_night6_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night6_raw_2688x1520_5376 | full | `3c09f9353da4a14790f34324a2fb6fb20d9811b39e0ed84061cf9c5897930f44` |
| image | khoipd_night6_raw_2688x1520_5376 | full_vga_709 | `69ade7051a4f71e3e4035c7afa11c9a7061c4e6ee4c0a2492c04bcb8c4f3ec8d` |
| image | khoipd_night6_raw_2688x1520_5376 | gtm_manual_qhd | `5641170beb788b96b5ee41bffb1ccc2a5b87e2361ba637cbfd21b1d6108a287f` |
| image | khoipd_night_raw_2688x1520_5376 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| image | khoipd_night_raw_2688x1520_5376 | full | `251494ac8747c84acb418ab973ef31a209eb9b3b14e579d48e58f38b37bb6d80` |
| image | khoipd_night_raw_2688x1520_5376 | full_vga_709 | `03d5cc289695d1dbcd7bb7c218b7671f15688e8d5223068337071e66722e9e20` |
| image | khoipd_night_raw_2688x1520_5376 | gtm_manual_qhd | `6c84062d8eb7874377e7cda98d82d23a98de04756be88fd719cc6bd143decd63` |
| sequence | sequence of 28 | basic | `ed5852644b6cbd638167c831a7c1d4539031f6823f5e111f0b85edeaf91294d4` |
| sequence | sequence of 28 | full | `6b0e3c738f7cd93fea9fb1bfe1c19b7c33efa9ef9a5f330fd5f4afdb41689d29` |
| sequence | sequence of 28 | full_vga_709 | `9ccfa6b374472046926bf9bd7c2779ece3acd7e4cb5243623f0fb87ae4cc7ea1` |
| sequence | sequence of 28 | gtm_manual_qhd | `e2ea89ce587307b1b2b2d28ecd038d3f0e93dba5e9778447f9405c0021b780ce` |
