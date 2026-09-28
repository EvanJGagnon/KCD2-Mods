# gen_recipes.py: AlchemyRecipe.xml -> alch_recipes.txt (compact, parsed at runtime by kcd2_alchemy.dll)
#   R <id> <baseIndex>   (Spiritus 0, Oil 1, Wine 2, Water 3 -> pour verb 13+base)
#   I <guid> <qty>
#   M <guid> <0|1>   IsMilled      W <guid> <turns>  WeakBoilingTime
#   S <guid> <turns> Strong        D <0|1> IsBaseDistilled   P <0|1> IsResultMilled
#   X  (condition we cannot drive -> recipe refused)
import zipfile,re,html
import os
# Game folder: set KCD2_DIR to override.
GAME = os.environ.get("KCD2_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2")
G=os.path.join(GAME,"Data","Tables.pak")
d=zipfile.ZipFile(G).read('Libs/Tables/minigame/AlchemyRecipe.xml').decode('utf-8')
BASE={'Spiritus':0,'Oil':1,'Wine':2,'Water':3}
out=[];n=0
for r in re.finditer(r'<AlchemyRecipe (.*?)</AlchemyRecipe>',d,re.S):
    head=r.group(1)
    rid=re.search(r'RecipeId="(\d+)"',head).group(1); base=re.search(r'BaseMaterial="(\w+)"',head).group(1)
    out.append('R %s %d'%(rid,BASE[base]))
    for g,q in re.findall(r'<AlchemyIngredient IngredientItemId="([^"]+)" Quantity="(\d+)"',head): out.append('I %s %s'%(g,q))
    for cond in re.findall(r'Condition="([^"]*)"',head):
        cond=html.unescape(cond)
        for fn,args in re.findall(r'(\w+)\(([^)]*)\)',cond):
            a=[x.strip().strip("'") for x in args.split(',')] if args.strip() else []
            if fn=='PotionBase' or fn=='IngredientCount': continue
            elif fn=='IsMilled': out.append('M %s %d'%(a[0],int(float(a[1]))))
            elif fn=='WeakBoilingTime': out.append('W %s %d'%(a[0],int(float(a[1]))))
            elif fn=='StrongBoilingTime': out.append('S %s %d'%(a[0],int(float(a[1]))))
            elif fn=='IsBaseDistilled': out.append('D %d'%int(float(a[0])))
            elif fn=='IsResultMilled': out.append('P %d'%int(float(a[0])))
            else: out.append('X %s'%fn)
    out.append('E'); n+=1
open('alch_recipes.txt','w').write('\n'.join(out)+'\n')
print('recipes',n,'lines',len(out),'undrivable',sum(1 for l in out if l.startswith('X')))
