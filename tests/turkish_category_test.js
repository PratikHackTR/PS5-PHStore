const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('frontend/src/app.js','utf8');
const c={Boolean,Array,state:{method:'ph',category:'turkish',searchQuery:'',games:[]}};
vm.createContext(c);
vm.runInContext(source.slice(source.indexOf('  const isDubbedGame'),source.indexOf('  const CATEGORIES'))+
 source.slice(source.indexOf('  function methodGames()'),source.indexOf('  function updateMethodCaption()'))+
 source.slice(source.indexOf('  function matchingGames()'),source.indexOf('  function setSearch(')),c);
const game=(id,platform,turkish,localization_type,packages=[],catalog_source='ph')=>({id,title:id,platform,turkish,localization_type,packages,catalog_source});
c.state.games=[game('dub-ps4','ps4',false,'dubbed'),game('dub-ps5','ps5',false,'dubbed'),
 game('package-dub','ps5',false,'none',[{localization_type:'dubbed'}]),game('text','ps2',true,'text'),
 game('ordinary','ps5',false,'none'),game('sp-dub','ps5',false,'dubbed',[],'sp')];
assert.deepEqual(Array.from(c.matchingGames(),x=>x.id),['dub-ps4','dub-ps5','package-dub','text']);
c.state.method='sp';assert.deepEqual(Array.from(c.matchingGames(),x=>x.id),['sp-dub']);
c.state.method='ph';c.state.category='ps5';assert.deepEqual(Array.from(c.matchingGames(),x=>x.id),['dub-ps5','package-dub','ordinary']);
c.state.category='turkish';
c.state.games=JSON.parse(fs.readFileSync('config/generated/embedded-catalog.json','utf8')).games;
assert(c.matchingGames().some(x=>x.id==='god-of-war-2018-tr-dublaj-update'));
console.log('TURKISH_CATEGORY_PASS game/package dubbing, PS4/PS5, PH/SP isolation, existing God of War; PH Turkish count='+c.matchingGames().length);
