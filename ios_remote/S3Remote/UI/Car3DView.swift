/*
 * Car3DView.swift — 姿态仪里的小车模型。SceneKit 是系统框架；车辆节点旋转，
 * 地面参照和相机固定。所有传感器值仅供显示，不反馈到驾驶控制。
 */
import SwiftUI
import SceneKit

struct Car3DView: View {
    var headingDeg: Double
    var rollDeg: Double
    var pitchDeg: Double
    var hasData: Bool
    var sourceLabel: String = "车体融合"

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 18).fill(Theme.ink)
            CarSceneView(headingDeg: headingDeg, rollDeg: rollDeg,
                         pitchDeg: pitchDeg)
                .allowsHitTesting(false)
                .accessibilityHidden(true)
            VStack {
                HStack {
                    Label("相对旋转", systemImage: "rotate.3d")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.7))
                    Spacer()
                    Text(hasData ? String(format: "%+.0f°", headingDeg) : "--°")
                        .font(Theme.mono(17, weight: .bold))
                        .foregroundStyle(.white)
                        .monospacedDigit()
                }
                Spacer()
                HStack {
                    Text(hasData ? sourceLabel : "等待 IMU 数据")
                    Spacer()
                    Text("3D ATTITUDE").tracking(1.2)
                }
                .font(Theme.mono(10))
                .foregroundStyle(.white.opacity(0.66))
            }
            .padding(16)
        }
        .clipShape(RoundedRectangle(cornerRadius: 18))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("三维小车姿态")
        .accessibilityValue(hasData
            ? String(format: "%@，相对旋转 %.0f 度，横滚 %.1f 度，俯仰 %.1f 度",
                     sourceLabel, headingDeg, rollDeg, pitchDeg)
            : "等待数据")
    }
}

private struct CarSceneView: UIViewRepresentable {
    let headingDeg: Double
    let rollDeg: Double
    let pitchDeg: Double

    func makeUIView(context: Context) -> SCNView {
        let view = SCNView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.autoenablesDefaultLighting = false
        view.antialiasingMode = .multisampling4X
        view.rendersContinuously = true
        let scene = SCNScene()
        scene.background.contents = UIColor.clear
        scene.rootNode.addChildNode(Self.car())
        scene.rootNode.addChildNode(Self.ground())

        let camera = SCNNode()
        camera.camera = SCNCamera()
        camera.camera?.fieldOfView = 39
        camera.position = SCNVector3(3.2, 2.55, -5.25)
        camera.look(at: SCNVector3(0, 0.60, 0))
        scene.rootNode.addChildNode(camera)
        view.pointOfView = camera

        let ambient = SCNNode()
        ambient.light = SCNLight()
        ambient.light?.type = .ambient
        ambient.light?.color = UIColor(white: 0.72, alpha: 1)
        scene.rootNode.addChildNode(ambient)
        let sun = SCNNode()
        sun.light = SCNLight()
        sun.light?.type = .omni
        sun.light?.color = UIColor(white: 0.95, alpha: 1)
        sun.position = SCNVector3(-3, 6, -4)
        scene.rootNode.addChildNode(sun)
        view.scene = scene
        return view
    }

    func updateUIView(_ view: SCNView, context: Context) {
        guard let car = view.scene?.rootNode.childNode(withName: "vehicle", recursively: false) else { return }
        let radians = Float.pi / 180
        // Body X前/Y左/Z上 -> SceneKit -Z前/-X左/+Y上。
        let yaw = simd_quatf(angle: Float(headingDeg) * radians, axis: SIMD3<Float>(0, 1, 0))
        let pitch = simd_quatf(angle: Float(pitchDeg) * radians, axis: SIMD3<Float>(1, 0, 0))
        let roll = simd_quatf(angle: Float(rollDeg) * radians, axis: SIMD3<Float>(0, 0, -1))
        SCNTransaction.begin()
        SCNTransaction.animationDuration = 0.10
        car.simdOrientation = yaw * pitch * roll
        SCNTransaction.commit()
    }

    private static func car() -> SCNNode {
        let root = SCNNode()
        root.name = "vehicle"
        let orange = UIColor(red: 0.91, green: 0.38, blue: 0.20, alpha: 1)
        let orangeDark = UIColor(red: 0.57, green: 0.22, blue: 0.16, alpha: 1)
        let glass = UIColor(red: 0.18, green: 0.34, blue: 0.42, alpha: 1)
        let cream = UIColor(red: 0.94, green: 0.91, blue: 0.81, alpha: 1)

        root.addChildNode(box(1.80, 0.39, 3.00, radius: 0.19,
                              color: orange, at: SCNVector3(0, 0.48, 0)))
        root.addChildNode(box(1.72, 0.08, 1.04, radius: 0.04,
                              color: orangeDark, at: SCNVector3(0, 0.70, -0.98)))
        root.addChildNode(box(1.48, 0.56, 1.31, radius: 0.11,
                              color: glass, at: SCNVector3(0, 0.92, 0.18)))
        root.addChildNode(box(1.57, 0.13, 1.42, radius: 0.09,
                              color: cream, at: SCNVector3(0, 1.25, 0.18)))
        // Windshield border and roof rails make the front unambiguous.
        root.addChildNode(box(1.54, 0.055, 0.07, radius: 0.02,
                              color: cream, at: SCNVector3(0, 1.06, -0.51)))
        for side: Float in [-1, 1] {
            root.addChildNode(box(0.055, 0.085, 1.30, radius: 0.02,
                                  color: orangeDark, at: SCNVector3(side * 0.77, 1.34, 0.18)))
            root.addChildNode(wheel(x: side * 0.97, z: -0.96))
            root.addChildNode(wheel(x: side * 0.97, z: 0.96))
            root.addChildNode(sphere(0.115, color: cream,
                                     at: SCNVector3(side * 0.62, 0.55, -1.53)))
            root.addChildNode(box(0.23, 0.10, 0.045, radius: 0.02,
                                  color: .systemRed, at: SCNVector3(side * 0.63, 0.51, 1.53)))
        }
        root.addChildNode(box(0.90, 0.12, 0.05, radius: 0.02,
                              color: .darkGray, at: SCNVector3(0, 0.43, -1.54)))
        // Bright nose stripe helps read yaw even when the car is nearly level.
        root.addChildNode(box(0.13, 0.026, 0.70, radius: 0.01,
                              color: cream, at: SCNVector3(0, 0.75, -1.08)))
        return root
    }

    private static func wheel(x: Float, z: Float) -> SCNNode {
        let root = SCNNode()
        root.position = SCNVector3(x, 0.34, z)
        let tire = SCNCylinder(radius: 0.35, height: 0.19)
        tire.radialSegmentCount = 20
        tire.firstMaterial = material(UIColor(white: 0.07, alpha: 1))
        let tireNode = SCNNode(geometry: tire)
        tireNode.eulerAngles.z = .pi / 2
        root.addChildNode(tireNode)
        let hub = SCNCylinder(radius: 0.16, height: 0.20)
        hub.firstMaterial = material(UIColor(white: 0.70, alpha: 1))
        let hubNode = SCNNode(geometry: hub)
        hubNode.eulerAngles.z = .pi / 2
        hubNode.position.x = x < 0 ? -0.03 : 0.03
        root.addChildNode(hubNode)
        return root
    }

    private static func ground() -> SCNNode {
        let root = SCNNode()
        let grid = UIColor(white: 0.55, alpha: 0.20)
        for radius: CGFloat in [1.4, 2.0, 2.6] {
            let ring = SCNTorus(ringRadius: radius, pipeRadius: 0.012)
            ring.firstMaterial = material(grid)
            let node = SCNNode(geometry: ring)
            node.position.y = -0.05
            root.addChildNode(node)
        }
        for angle in [Float.zero, Float.pi / 2] {
            let line = box(0.012, 0.012, 5.2, radius: 0,
                           color: grid, at: SCNVector3(0, -0.05, 0))
            line.eulerAngles.y = angle
            root.addChildNode(line)
        }
        return root
    }

    private static func box(_ w: CGFloat, _ h: CGFloat, _ l: CGFloat,
                            radius: CGFloat, color: UIColor, at p: SCNVector3) -> SCNNode {
        let geometry = SCNBox(width: w, height: h, length: l, chamferRadius: radius)
        geometry.firstMaterial = material(color)
        let node = SCNNode(geometry: geometry)
        node.position = p
        return node
    }

    private static func sphere(_ radius: CGFloat, color: UIColor, at p: SCNVector3) -> SCNNode {
        let geometry = SCNSphere(radius: radius)
        geometry.firstMaterial = material(color)
        let node = SCNNode(geometry: geometry)
        node.position = p
        return node
    }

    private static func material(_ color: UIColor) -> SCNMaterial {
        let m = SCNMaterial()
        m.diffuse.contents = color
        m.lightingModel = .blinn
        return m
    }
}
