//
// FBXを読み込む
//   Animation対応(その２)
//
// TODO:プロジェクト設定にFBX SDKのincludeファイルへのパスと
//      libファイルへのパスを追加する
//
// SOURCE:http://ramemiso.hateblo.jp/entry/2014/06/21/150405
// SOURCE:http://help.autodesk.com/view/FBX/2016/ENU/?guid=__files_GUID_105ED19A_9A5A_425E_BFD7_C1BBADA67AAB_htm
// SOURCE:http://shikemokuthinking.blogspot.jp/search/label/FBX
//

#ifdef _MSC_VER
// TIPS:FBX SDKはDLLを使う
#define FBXSDK_SHARED
#endif

#include <cinder/app/AppNative.h>
#include <cinder/Camera.h>
#include <cinder/Arcball.h>
#include <cinder/TriMesh.h>
#include <cinder/gl/gl.h>
#include <cinder/gl/Texture.h>
#include <cinder/gl/Light.h>
#include <cinder/gl/Material.h>
#include <cinder/ip/Flip.h>
#include <boost/optional.hpp>
#include <boost/filesystem.hpp>
#include <fbxsdk.h>                // FBX SDK

using namespace ci;
using namespace ci::app;


#ifdef _MSC_VER
// cp932 → UTF-8
std::string getUTF8Path(const std::string& path)
{
	// 相対パス → 絶対パス
	char fullPath[512];
	_fullpath(fullPath, path.c_str(), 512);

	// cp932 → UTF8
	char* path_utf8;
	FbxAnsiToUTF8(fullPath, path_utf8);

	// char* → std::string
	std::string coverted_path(path_utf8);
	// FBX SDK内部で確保されたメモリは専用の関数で解放
	FbxFree(path_utf8);

	return coverted_path;
}
#endif

// FBX SDKサンプルから拝借
// Get the geometry offset to a node. It is never inherited by the children.
FbxAMatrix GetGeometry(FbxNode* pNode)
{
  const FbxVector4 lT = pNode->GetGeometricTranslation(FbxNode::eSourcePivot);
  const FbxVector4 lR = pNode->GetGeometricRotation(FbxNode::eSourcePivot);
  const FbxVector4 lS = pNode->GetGeometricScaling(FbxNode::eSourcePivot);

  return FbxAMatrix(lT, lR, lS);
}


// FbxAMatrix → Matrix44
Matrix44f getMatrix44(const FbxAMatrix& matrix)
{
  const double* p = (const double*)matrix;

  Matrix44f m(p[0], p[1], p[2], p[3],
              p[4], p[5], p[6], p[7],
              p[8], p[9], p[10], p[11],
              p[12], p[13], p[14], p[15]);

  return m;
}


// スケルタルアニメーション用の情報
struct Skin
{
  bool has_skins;
  std::vector<std::vector<float>> weights;
  std::vector<FbxAMatrix> base_inv;
};


// 表示用のメッシュ
struct Mesh
{
  TriMesh tri_mesh;

  Skin skin;
  TriMesh deformed_mesh;
};


// FBXから取り出した情報を保持しておくための定義
// マテリアル
struct Material
{
  gl::Material material;
  boost::optional<gl::Texture> texture;
};


class TestProjectApp
  : public AppNative
{
  enum {
    WINDOW_WIDTH  = 800,
    WINDOW_HEIGHT = 600,
  };

  CameraPersp camera;

  Arcball arcball;


  // FBX情報
  FbxManager* manager;
  FbxScene* scene;
  FbxNode* root_node;


  // 表示物の情報を名前で管理
  std::map<std::string, Mesh> meshes;
  std::map<std::string, Material> materials;


  gl::Light* light;

  // アニメーションの経過時間
  double animation_time = 0.0;
  double animation_start;
  double animation_stop;

  // シーン内のアニメーション数
  int animation_stack_count;
  int current_animation_stack = 0;


  void setAnimation(const int index);

  Mesh createMesh(FbxMesh* mesh);
  Skin createSkin(FbxMesh* mesh);
  Material createMaterial(FbxSurfaceMaterial* material);

  TriMesh getDeformedTriMesh(FbxMesh* mesh, const Mesh& src_mesh, FbxAMatrix& parent_matrix, FbxTime& time);
  const TriMesh& getTriMesh(FbxMesh* mesh, Mesh& src_mesh, FbxAMatrix& parent_matix, FbxTime& time);

  // TIPS:FBXのデータを再帰的に描画する
  void draw(FbxNode* node, FbxTime& time);


public:
  void prepareSettings(Settings* settings);

  void mouseDown(MouseEvent event);
  void mouseDrag(MouseEvent event);

  void setup();
  void draw();
};


// FbxMesh→Mesh
Mesh TestProjectApp::createMesh(FbxMesh* mesh)
{
  Mesh mesh_info;

  {
    // 頂点座標
    int indexCount = mesh->GetPolygonVertexCount();
    console() << "index:" << indexCount << std::endl;

    // TIPS:頂点配列から展開してTriMeshに格納している(T^T)
    int* index = mesh->GetPolygonVertices();
    for (int i = 0; i < indexCount; ++i)
    {
      auto controlPoint = mesh->GetControlPointAt(index[i]);
      mesh_info.tri_mesh.appendVertex(Vec3f(controlPoint[0], controlPoint[1], controlPoint[2]));
    }

    for (int i = 0; i < indexCount; i += 3)
    {
      mesh_info.tri_mesh.appendTriangle(i, i + 1, i + 2);
    }
  }

  {
    // 頂点法線
    FbxArray<FbxVector4> normals;
    mesh->GetPolygonVertexNormals(normals);

    console() << "normals:" << normals.Size() << std::endl;

    for (int i = 0; i < normals.Size(); ++i)
    {
      const FbxVector4& n = normals[i];
      mesh_info.tri_mesh.appendNormal(Vec3f(n[0], n[1], n[2]));
    }
  }

  {
    // UV
    FbxStringList uvsetName;
    mesh->GetUVSetNames(uvsetName);

    if (uvsetName.GetCount() > 0)
    {
      // 最初のUVセットを取り出す
      console() << "UV SET:" << uvsetName.GetStringAt(0) << std::endl;

      FbxArray<FbxVector2> uvsets;
      mesh->GetPolygonVertexUVs(uvsetName.GetStringAt(0), uvsets);

      console() << "UV:" << uvsets.Size() << std::endl;

      for (int i = 0; i < uvsets.Size(); ++i)
      {
        const FbxVector2& uv = uvsets[i];
        mesh_info.tri_mesh.appendTexCoord(Vec2f(uv[0], uv[1]));
      }
    }
  }

  // スケルタルアニメーション情報を取得
  mesh_info.skin = createSkin(mesh);

  return mesh_info;
}

// スケルタルアニメーションの情報を生成
Skin TestProjectApp::createSkin(FbxMesh* mesh)
{
  Skin skin_info;
  skin_info.has_skins = false;

  auto skinCount = mesh->GetDeformerCount(FbxDeformer::eSkin);
  if (skinCount == 0)
  {
    // スケルタルアニメーションなし
    console() << "No skeltal animation." << std::endl;
    return skin_info;
  }

  // eSkin形式のデフォーマーが２つ以上存在する場合がある
  console() << "deformer:" << skinCount << std::endl;

  // ０番目のeSkin形式のデフォーマーを取り出す
  FbxSkin* skin = static_cast<FbxSkin*>(mesh->GetDeformer(0, FbxDeformer::eSkin));
  int clusterCount = skin->GetClusterCount();

  console() << "cluster:" << clusterCount << std::endl;

  if (clusterCount == 0)
  {
    // FIXME:影響を与えるクラスタが無い??
    console() << "No cluster." << std::endl;
    return skin_info;
  }

  skin_info.has_skins = true;
  skin_info.weights.resize(mesh->GetPolygonVertexCount());

  for (auto& weights : skin_info.weights)
  {
    weights.resize(clusterCount);
    std::fill(std::begin(weights), std::end(weights), 0.0);
  }

  // 初期行列の逆行列
  skin_info.base_inv.resize(clusterCount);

  int vtx_indexCount = mesh->GetPolygonVertexCount();
  int* vtx_index     = mesh->GetPolygonVertices();

  for (int i = 0; i < clusterCount; ++i)
  {
    FbxCluster* cluster = skin->GetCluster(i);

    // eNormalizeしか対応しない
    assert(cluster->GetLinkMode() == FbxCluster::eNormalize);

    int indexCount  = cluster->GetControlPointIndicesCount();
    int* indices    = cluster->GetControlPointIndices();
    double* weights = cluster->GetControlPointWeights();

    // ウェイトを取り出す
    for (int j = 0; j < indexCount; ++j)
    {
      double w = weights[j];

      // コントロールポイント→頂点配列位置
      for (int k = 0; k < vtx_indexCount; ++k)
      {
        if (vtx_index[k] == indices[j])
        {
          skin_info.weights[k][i] = w;
        }
      }
    }

    // 初期状態の逆行列を計算しておく
    // FbxAMatrix m;
    // cluster->GetTransformLinkMatrix(m);
    // skin_info.base_inv[i] = m.Inverse();
  }

  return skin_info;
}


// ウエイト情報を元にTriMeshの頂点を変形
TriMesh TestProjectApp::getDeformedTriMesh(FbxMesh* mesh, const Mesh& src_mesh, FbxAMatrix& parent_matrix, FbxTime& time)
{
  TriMesh dst_mesh(src_mesh.tri_mesh);

  FbxSkin* skin = static_cast<FbxSkin*>(mesh->GetDeformer(0, FbxDeformer::eSkin));
  int clusterCount = skin->GetClusterCount();

  std::vector<Matrix44f> matricies(clusterCount);

  // 逆行列を取得
  FbxAMatrix inv = parent_matrix.Inverse();

  for (int i = 0; i < clusterCount; ++i)
  {
    // 影響を受ける行列を取り出す
    // FIXME:FBX SDKサンプルコードそのまま。効率悪いので要修正
    FbxCluster* cluster = skin->GetCluster(i);

    FbxAMatrix lReferenceGlobalInitPosition;
    cluster->GetTransformMatrix(lReferenceGlobalInitPosition);

		FbxAMatrix lReferenceGeometry = GetGeometry(mesh->GetNode());
		lReferenceGlobalInitPosition *= lReferenceGeometry;

    FbxAMatrix lClusterGlobalInitPosition;
		cluster->GetTransformLinkMatrix(lClusterGlobalInitPosition);

    FbxNode* node = cluster->GetLink();
    FbxAMatrix lClusterGlobalCurrentPosition = node->EvaluateGlobalTransform(time);

    FbxAMatrix lClusterRelativeInitPosition = lClusterGlobalInitPosition.Inverse() * lReferenceGlobalInitPosition;
		FbxAMatrix lClusterRelativeCurrentPositionInverse = inv * lClusterGlobalCurrentPosition;

    matricies[i] = getMatrix44(lClusterRelativeCurrentPositionInverse * lClusterRelativeInitPosition);
  }


  // 頂点座標を変換
  const auto& src_vtx = src_mesh.tri_mesh.getVertices();
  auto& dst_vtx       = dst_mesh.getVertices();
  const auto& src_norm = src_mesh.tri_mesh.getNormals();
  auto& dst_norm       = dst_mesh.getNormals();

  for (size_t i = 0; i < src_mesh.skin.weights.size(); ++i)
  {
    Matrix44f m = matricies[0] * src_mesh.skin.weights[i][0];
    for (int j = 1; j < clusterCount; ++j)
    {
      if (src_mesh.skin.weights[i][j] == 0.0) continue;

      m += matricies[j] * src_mesh.skin.weights[i][j];
    }

    // 頂点座標と法線を変換
    dst_vtx[i]  = m.transformPoint(src_vtx[i]);
    if (src_mesh.tri_mesh.hasNormals()) dst_norm[i] = m.transformVec(src_norm[i]);
  }

  return dst_mesh;
}


const TriMesh& TestProjectApp::getTriMesh(FbxMesh* mesh, Mesh& src_mesh, FbxAMatrix& parent_matrix, FbxTime& time)
{
  if (src_mesh.skin.has_skins)
  {
    src_mesh.deformed_mesh = getDeformedTriMesh(mesh, src_mesh, parent_matrix, time);
    return src_mesh.deformed_mesh;
  }
  else
  {
    return src_mesh.tri_mesh;
  }
}


// FbxSurfaceMaterial → Material
Material TestProjectApp::createMaterial(FbxSurfaceMaterial* material)
{
  Material mat;

  ColorA ambient(0, 0, 0);
  ColorA diffuse(1, 1, 1);
  ColorA emissive(0, 0, 0);
  ColorA specular(0, 0, 0);
  float shininess = 80.0;

  // マテリアルから必要な情報を取り出す
  {
    FbxProperty prop = material->FindProperty(FbxSurfaceMaterial::sAmbient);
    if (prop.IsValid())
    {
      const auto& color = prop.Get<FbxDouble3>();
      ambient = ColorA(color[0], color[1], color[2]);
      console() << "ambient:" << ambient << std::endl;
    }
  }
  {
    FbxProperty prop = material->FindProperty(FbxSurfaceMaterial::sDiffuse);
    if (prop.IsValid())
    {
      const auto& color = prop.Get<FbxDouble3>();
      diffuse = ColorA(color[0], color[1], color[2]);
      console() << "diffuse:" << diffuse << std::endl;
    }
  }
  {
    FbxProperty prop = material->FindProperty(FbxSurfaceMaterial::sEmissive);
    if (prop.IsValid())
    {
      const auto& color = prop.Get<FbxDouble3>();
      emissive = ColorA(color[0], color[1], color[2]);
      console() << "emissive:" << emissive << std::endl;
    }
  }
  {
    FbxProperty prop = material->FindProperty(FbxSurfaceMaterial::sSpecular);
    if (prop.IsValid())
    {
      const auto& color = prop.Get<FbxDouble3>();
      specular = ColorA(color[0], color[1], color[2]);
      console() << "specular:" << specular << std::endl;
    }
  }
  {
    FbxProperty prop = material->FindProperty(FbxSurfaceMaterial::sShininess);
    if (prop.IsValid())
    {
      shininess = prop.Get<FbxDouble>();
      console() << "shininess:" << shininess << std::endl;
    }
  }

  mat.material = gl::Material(ambient, diffuse, specular, shininess, emissive);

  {
    // テクスチャ(Diffuseにアタッチされているテクスチャを取得)
    FbxProperty prop = material->FindProperty(FbxSurfaceMaterial::sDiffuse);
    if (prop.GetSrcObjectCount<FbxFileTexture>() > 0)
    {
      // TIPS:複数テクスチャが適用されてても無視して１枚目を使う
      FbxFileTexture* texture = prop.GetSrcObject<FbxFileTexture>(0);
      if (texture)
      {
        // TIPS:テクスチャのパスは絶対パスで格納されているので
        //      余分なディレクトリ名とかを削除している
        // TODO:複数のマテリアルで同じテクスチャが指定されている場合に対応
        fs::path name = texture->GetFileName();

        // TIPS:画像を読み込んで上下反転
        Surface surface = loadImage(loadAsset(name.filename()));
        ip::flipVertical(&surface);

        mat.texture = surface;
        mat.texture->setWrap(GL_REPEAT, GL_REPEAT);

        console() << "texture:" << name.filename() << std::endl;
      }
    }
  }

  return mat;
}


void TestProjectApp::setAnimation(const int index)
{
  auto* stack = scene->GetSrcObject<FbxAnimStack>(index);
  // assert(stack);
  if (!stack) return;

  animation_start = stack->LocalStart.Get().GetSecondDouble();
  animation_stop  = stack->LocalStop.Get().GetSecondDouble();
  console() << "Duration:" << animation_start << "-" << animation_stop << std::endl;

  animation_time = animation_start;

  scene->SetCurrentAnimationStack(stack);
}


// 描画
void TestProjectApp::draw(FbxNode* node, FbxTime& time)
{
  // TIPS:見えているノードのみ描画(物理演算用の描画をスキップ)
  if (node->GetVisibility())
  {
    // 行列
    FbxAMatrix& matrix = node->EvaluateGlobalTransform(time);

    // １つのノードに複数の属性が関連づけられる
    int attr_count = node->GetNodeAttributeCount();
    for (int i = 0; i < attr_count; ++i)
    {
      FbxNodeAttribute* attr = node->GetNodeAttributeByIndex(i);
      switch(attr->GetAttributeType())
      {
      case FbxNodeAttribute::eMesh:
        {
          gl::pushModelView();

          glMatrixMode(GL_MODELVIEW);
          glMultMatrixd(matrix);

          // 描画に使うメッシュとマテリアルを特定
          FbxMesh* mesh = static_cast<FbxMesh*>(attr);
          auto& mesh_info = meshes.at(mesh->GetName());

          // スケルタルアニメーションを適用
          const auto& tri_mesh = getTriMesh(mesh, mesh_info, matrix, time);

          FbxSurfaceMaterial* material = node->GetMaterial(i);
          const auto& mat = materials.at(material->GetName());

          mat.material.apply();
          if (mat.texture)
          {
            mat.texture->enableAndBind();
          }

          gl::draw(tri_mesh);

          if (mat.texture)
          {
            mat.texture->unbind();
            mat.texture->disable();
          }

          gl::popModelView();
        }
        break;

      default:
        break;
      }
    }
  }

  // 子供のノードを再帰で描画
  int childCount = node->GetChildCount();
  for (int i = 0; i < childCount; ++i)
  {
    FbxNode* child = node->GetChild(i);
    draw(child, time);
  }
}


void TestProjectApp::prepareSettings(Settings* settings)
{
  settings->setWindowSize(WINDOW_WIDTH, WINDOW_HEIGHT);
}

void TestProjectApp::setup()
{
  camera = CameraPersp(getWindowWidth(), getWindowHeight(),
                       35.0f,
                       0.1f, 100.0f);

  // TIPS:setEyePoint → setCenterOfInterestPoint の順で初期化すること
  camera.setEyePoint(Vec3f(0.0f, 0.0f, -4.0f));
  camera.setCenterOfInterestPoint(Vec3f(0.0f, 0.0f, 0.0f));

  light = new gl::Light(gl::Light::DIRECTIONAL, 0);
  light->setAmbient(Color(0.5f, 0.5f, 0.5f));
  light->setDiffuse(Color(0.8f, 0.8f, 0.8f));
  light->setSpecular(Color(0.8f, 0.8f, 0.8f));
  light->setDirection(Vec3f(0.0f, 0.0f, -1.0f));

  arcball = Arcball(getWindowSize());
  arcball.setRadius(150.0f);

  gl::enableDepthRead();
  gl::enableDepthWrite();
  gl::enable(GL_CULL_FACE);
  gl::enable(GL_NORMALIZE);
  gl::enableAlphaBlending();


  // FBXSDK生成
  manager = FbxManager::Create();
  assert(manager);

  // 読み込み機能を生成
  auto* importer = FbxImporter::Create(manager, "");
  assert(importer);

  // FBXファイルを読み込む
  // TIPS: getAssetPathは、assets内のファイルを探して、フルパスを取得する
  std::string path = getAssetPath("test.fbx").string();
#ifdef _MSC_VER
  path = getUTF8Path(path);
#endif

  //TIPS: std::string → char*
  if (!importer->Initialize(path.c_str()))
  {
    console() << "FBX:can't open " << path << std::endl;
  }

  // 読み込み用のシーンを生成
  scene = FbxScene::Create(manager, "");
  assert(scene);

  // ファイルからシーンへ読み込む
  importer->Import(scene);

  console() << "Imported." << std::endl;

  // FbxImporterはもう使わないのでここで破棄
  importer->Destroy();

  FbxGeometryConverter geometryConverter(manager);
  // TIPS:あらかじめポリゴンを全て三角形化しておく
  geometryConverter.Triangulate(scene, true);
  geometryConverter.RemoveBadPolygonsFromMeshes(scene);

  // TIPS:マテリアルごとにメッシュを分離
  geometryConverter.SplitMeshesPerMaterial(scene, true);

  console() << "Converted." << std::endl;

  // FBX内の構造を取得しておく
  root_node = scene->GetRootNode();
  assert(root_node);

  {
    // シーンに含まれるメッシュの解析
    auto meshCount = scene->GetSrcObjectCount<FbxMesh>();
    console() << "meshCount:" << meshCount << std::endl;

    for (int i = 0; i < meshCount; ++i)
    {
      auto* mesh = scene->GetSrcObject<FbxMesh>(i);
      std::string name = mesh->GetName();

      if (meshes.count(name)) continue;

      Mesh mesh_info = createMesh(mesh);

      meshes.insert({ name, mesh_info });
    }
  }

  {
    // シーンに含まれるマテリアルの解析
    auto materialCount = scene->GetMaterialCount();
    console() << "material:" << materialCount << std::endl;

    for (int i = 0; i < materialCount; ++i)
    {
      FbxSurfaceMaterial* material = scene->GetMaterial(i);
      std::string name = material->GetName();

      if (materials.count(name)) continue;

      Material mat = createMaterial(material);
      materials.insert({ name, mat });
    }
  }

  // アニメーションに必要な情報を収集
  animation_stack_count = scene->GetSrcObjectCount<FbxAnimStack>();
  console() << "Anim:" << animation_stack_count << std::endl;
  setAnimation(current_animation_stack);

  console() << "Pose:" << scene->GetPoseCount() << std::endl;

  // FBS SDKを破棄
  // manager->Destroy();
}


void TestProjectApp::mouseDown(MouseEvent event)
{
  if (event.isLeft())
  {
    Vec2i pos = event.getPos();
    arcball.mouseDown(pos);
  }
  else if (event.isRight())
  {
    current_animation_stack = (current_animation_stack + 1) % animation_stack_count;
    setAnimation(current_animation_stack);
  }
}

void TestProjectApp::mouseDrag(MouseEvent event)
{
  if (!event.isLeftDown()) return;

  Vec2i pos = event.getPos();
  arcball.mouseDrag(pos);
}


void TestProjectApp::draw()
{
  // アニメーション経過時間を進める
  animation_time += 1 / 60.0;
  if (animation_time > animation_stop)
  {
    animation_time = animation_start + animation_time - animation_stop;
  }


	gl::clear( Color( 0.5, 0.5, 0.5 ) );

  gl::setMatrices(camera);
  gl::enable(GL_LIGHTING);
  light->enable();

  {
    Quatf rotate = arcball.getQuat();
    Matrix44f m = rotate.toMatrix44();
    gl::multModelView(m);
  }

  // gl::translate(0, -1.0, 0);
  // gl::scale(0.005, 0.005, 0.005);

  // 描画
  FbxTime time;
  time.SetSecondDouble(animation_time);
  draw(root_node, time);

  light->disable();
}

CINDER_APP_NATIVE( TestProjectApp, RendererGl )
