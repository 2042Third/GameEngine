#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Layer.h"

#include <vector>

namespace Strata
{

	// Owns its layers. Regular layers sit below overlays; each group keeps insertion order.
	class LayerStack
	{
	public:
		LayerStack() = default;
		~LayerStack();

		LayerStack(const LayerStack&) = delete;
		LayerStack& operator=(const LayerStack&) = delete;

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);
		// Detaches the layer and returns ownership to the caller.
		void PopLayer(Layer* layer);
		void PopOverlay(Layer* overlay);
		// Detaches (top-down) and deletes every layer.
		void Clear();

		std::vector<Layer*>::iterator begin() { return m_Layers.begin(); }
		std::vector<Layer*>::iterator end() { return m_Layers.end(); }
		std::vector<Layer*>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		std::vector<Layer*>::reverse_iterator rend() { return m_Layers.rend(); }

		size_t GetSize() const { return m_Layers.size(); }
	private:
		std::vector<Layer*> m_Layers;
		uint32_t m_LayerInsertIndex = 0;
	};

}
